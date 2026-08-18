# Regression test for #15385:
# RESTORE of a Vector Set whose HNSW serialization reuses the same node ID
# must be rejected. Accepting the payload leaves a non-reciprocal pointer
# graph after link resolution, and VREM + VLINKS can use-after-free / crash.
#
# Copyright (c) 2009-Present, Redis Ltd.
# All rights reserved.
#
# Licensed under your choice of (a) the Redis Source Available License 2.0
# (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
# GNU Affero General Public License v3 (AGPLv3).

from test import TestCase
import struct
import redis


def enc_len(value):
    """Encode a length the way rdbSaveLen() does for small integers."""
    if value < 64:
        return bytes([value])
    if value < (1 << 14):
        return bytes([0x40 | (value >> 8), value & 0xff])
    if value <= 0xffffffff:
        return b"\x80" + struct.pack(">I", value)
    return b"\x81" + struct.pack(">Q", value)


def module_uint(value):
    """RDB_MODULE_OPCODE_UINT + rdbSaveLen(value)."""
    return enc_len(2) + enc_len(value)


def module_string(data):
    """RDB_MODULE_OPCODE_STRING + raw string (length-prefixed)."""
    if isinstance(data, str):
        data = data.encode()
    return enc_len(5) + enc_len(len(data)) + data


def serialized_node(element, node_id, links, vector, level=0):
    """
    Serialize one HNSW node the way VectorSetRdbSave() / hnsw_serialize_node()
    would for FP32 (HNSW_QUANT_NONE).

    params layout (version 1), per layer 0..level:
      [id, version|level, (num_links, max_links, link_ids..., worst_pack)..., l2|range]

    'links' is applied on layer 0 only; higher layers are empty (valid for
    crafting illegal levels / isolated high-level nodes).
    """
    m = 16
    params = [
        node_id,
        (1 << 24) | (level & 0xFF),  # version=1, level
    ]
    for i in range(level + 1):
        layer_links = links if i == 0 else []
        params.append(len(layer_links))
        params.append(m * 2 if i == 0 else m)
        params.extend(layer_links)
        # worst_idx in low 32 bits, worst_distance bits in high 32
        params.append(0)
    # l2 = 1.0f in low 32 bits, quants_range = 0 in high 32
    params.append(0x3F800000)

    out = module_string(element)
    out += module_string(struct.pack(f"<{len(vector)}f", *vector))
    out += module_uint(len(params))
    for param in params:
        out += module_uint(param)
    return out


def build_vectorset_dump(nodes, module_id, dim=2, quant=0, M=16, rdb_version=14):
    """
    Build a RESTORE/DUMP payload for a Vector Set (RDB_TYPE_MODULE_2).

    nodes: list of (element, node_id, links, vector_floats)
           or (element, node_id, links, vector_floats, level)
    A zero CRC footer is accepted by verifyDumpPayload().
    """
    body = bytearray([7])  # RDB_TYPE_MODULE_2
    body += enc_len(module_id)
    body += module_uint(dim)
    body += module_uint(len(nodes))
    hnsw_config = (quant & 0xFF) | ((M & 0xFFFF) << 8)
    body += module_uint(hnsw_config)
    body += module_uint(0)  # save_flags: no proj matrix, no attribs

    for node in nodes:
        if len(node) == 5:
            element, node_id, links, vector, level = node
        else:
            element, node_id, links, vector = node
            level = 0
        body += serialized_node(element, node_id, links, vector, level=level)

    body += enc_len(0)  # RDB_MODULE_OPCODE_EOF
    body += struct.pack("<H", rdb_version)
    body += b"\x00" * 8  # no checksum
    return bytes(body)


class RestoreDuplicateHNSWID(TestCase):
    def getname(self):
        return "RESTORE rejects corrupt Vector Set payloads"

    def estimated_runtime(self):
        return 0.5

    def _live_module_id(self):
        """Obtain the module type id Redis actually writes into DUMP."""
        # Create a tiny legitimate vector set and read the DUMP header.
        key = f"{self.test_key}:probe"
        self.redis.delete(key)
        self.redis.execute_command(
            "VADD", key, "VALUES", 2, "1.0", "0.0", "probe-a"
        )
        dump = self.redis.execute_command("DUMP", key)
        self.redis.delete(key)

        # type(1) + module id as rdb length encoding
        assert dump[0] == 7, f"expected RDB_TYPE_MODULE_2, got {dump[0]}"
        b = dump[1]
        typ = (b & 0xC0) >> 6
        if typ == 0:
            return b & 0x3F
        if typ == 1:
            return ((b & 0x3F) << 8) | dump[2]
        if b == 0x80:
            return struct.unpack(">I", dump[2:6])[0]
        if b == 0x81:
            return struct.unpack(">Q", dump[2:10])[0]
        raise AssertionError(f"unexpected module id length encoding 0x{b:02x}")

    def test(self):
        module_id = self._live_module_id()

        # Positive control: unique IDs + reciprocal links must RESTORE cleanly
        # and remain usable.
        valid_nodes = [
            ("a", 1, [2], [1.0, 0.0]),
            ("b", 2, [1, 3], [0.0, 1.0]),
            ("c", 3, [2], [1.0, 1.0]),
        ]
        valid_payload = build_vectorset_dump(valid_nodes, module_id)
        good_key = f"{self.test_key}:good"
        self.redis.delete(good_key)
        assert (
            self.redis.execute_command("RESTORE", good_key, 0, valid_payload) == b"OK"
        ), "valid Vector Set payload should RESTORE"
        assert self.redis.execute_command("TYPE", good_key) == b"vectorset"
        assert self.redis.execute_command("VCARD", good_key) == 3
        # Round-trip through DUMP/RESTORE once more to ensure we did not
        # break legitimate serialization.
        dumped = self.redis.execute_command("DUMP", good_key)
        self.redis.delete(good_key)
        assert (
            self.redis.execute_command("RESTORE", good_key, 0, dumped) == b"OK"
        ), "legitimate DUMP should RESTORE after a round-trip"
        assert self.redis.execute_command("VCARD", good_key) == 3
        self.redis.delete(good_key)

        # Malicious payload from #15385:
        #   a: id=1 -> links to 2
        #   b: id=1 (duplicate), no links
        #   c: id=2 -> links to 1
        # ID-level reciprocity sees pair {1,2} twice and would accept it;
        # pointer resolution maps c->1 to the first id=1 node (a or b
        # depending on insert order into the table), leaving a non-reciprocal
        # graph. After the fix, RESTORE must refuse this as corruption.
        malicious_nodes = [
            ("a", 1, [2], [1.0, 0.0]),
            ("b", 1, [], [0.0, 1.0]),
            ("c", 2, [1], [1.0, 1.0]),
        ]
        malicious_payload = build_vectorset_dump(malicious_nodes, module_id)
        bad_key = f"{self.test_key}:bad"
        self.redis.delete(bad_key)

        self._assert_restore_rejected(bad_key, malicious_payload)

        # id==0 is the in-memory auto-assign sentinel in hnsw_node_new().
        # Serialized payloads must never use it (legitimate nodes start at 1).
        id0_nodes = [
            ("a", 0, [2], [1.0, 0.0]),
            ("b", 1, [], [0.0, 1.0]),
            ("c", 2, [1], [1.0, 1.0]),
        ]
        self._assert_restore_rejected(
            f"{self.test_key}:id0",
            build_vectorset_dump(id0_nodes, module_id),
        )

        # Triple duplicate IDs with no links — still corruption.
        triple = [
            ("a", 5, [], [1.0, 0.0]),
            ("b", 5, [], [0.0, 1.0]),
            ("c", 5, [], [1.0, 1.0]),
        ]
        self._assert_restore_rejected(
            f"{self.test_key}:triple",
            build_vectorset_dump(triple, module_id),
        )

        # Level above HNSW_MAX_LEVEL (16) never occurs in legitimate data.
        high_level = [
            ("a", 1, [2], [1.0, 0.0], 32),
            ("b", 2, [1], [0.0, 1.0], 0),
        ]
        self._assert_restore_rejected(
            f"{self.test_key}:level",
            build_vectorset_dump(high_level, module_id),
        )

        # Duplicate element names leave an orphan HNSW node if accepted.
        dup_name = [
            ("same", 1, [2], [1.0, 0.0]),
            ("same", 2, [1], [0.0, 1.0]),
        ]
        self._assert_restore_rejected(
            f"{self.test_key}:dupname",
            build_vectorset_dump(dup_name, module_id),
        )

        assert self.redis.ping() is True, "server must stay up after rejecting payloads"

    def _assert_restore_rejected(self, key, payload):
        self.redis.delete(key)
        try:
            self.redis.execute_command("RESTORE", key, 0, payload)
            assert False, f"RESTORE of corrupt payload should fail (key={key})"
        except redis.exceptions.ResponseError as e:
            assert "Bad data format" in str(e), f"unexpected error for {key}: {e}"
        assert self.redis.exists(key) == 0, f"failed RESTORE must not create {key}"
