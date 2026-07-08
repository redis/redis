from test import TestCase
import struct
import redis


def enc_len(value):
    """Encode a length using the RDB length encoding."""
    if value < 64:
        return bytes([value])
    if value < (1 << 14):
        return bytes([0x40 | (value >> 8), value & 0xff])
    if value <= 0xffffffff:
        return b"\x80" + struct.pack("!I", value)
    return b"\x81" + struct.pack("!Q", value)


def load_len(buf, pos):
    """Decode a length previously encoded with enc_len()."""
    b = buf[pos]
    pos += 1
    typ = (b & 0xC0) >> 6
    if typ == 0:
        return b & 0x3f, pos
    if typ == 1:
        return ((b & 0x3f) << 8) | buf[pos], pos + 1
    if b == 0x80:
        return struct.unpack("!I", buf[pos:pos + 4])[0], pos + 4
    if b == 0x81:
        return struct.unpack("!Q", buf[pos:pos + 8])[0], pos + 8
    raise ValueError("unexpected encoded length")


def module_uint(value):
    return enc_len(2) + enc_len(value)


def module_string(data):
    return enc_len(5) + enc_len(len(data)) + data


def serialized_node(element, node_id, links):
    """Serialize a single level-0 HNSW node the way VectorSetRdbSave() does."""
    m = 16
    params = [
        node_id,
        (1 << 24),       # version=1, level=0
        len(links),
        m * 2,           # layer-0 max_links
    ]
    params.extend(links)
    params.append(0)             # worst_idx=0, worst_distance=0
    params.append(0x3f800000)    # l2=1.0, qrange=0

    out = module_string(element.encode())
    out += module_string(struct.pack("<ff", 1.0, 0.0))
    out += module_uint(len(params))
    for param in params:
        out += module_uint(param)
    return out


def build_dup_id_payload(module_id, rdb_version):
    """Craft a Vector Set DUMP payload with two nodes sharing HNSW ID 1.

    The duplicate ID lets the reciprocal-links check pass at the ID level
    while the resolved pointer graph is non-reciprocal (a -> c, c -> b),
    which used to leave a dangling link after VREM and crash on VLINKS.
    """
    body = bytearray([7]) + enc_len(module_id)  # RDB_TYPE_MODULE_2 + module id
    body += module_uint(2)          # vector dimension
    body += module_uint(3)          # elements
    body += module_uint(16 << 8)    # quant_type=f32, M=16
    body += module_uint(0)          # save_flags

    body += serialized_node("a", 1, [2])
    body += serialized_node("b", 1, [])
    body += serialized_node("c", 2, [1])

    body += enc_len(0)                          # module EOF opcode
    body += struct.pack("<H", rdb_version)      # DUMP footer version
    body += b"\x00" * 8                         # zero checksum: accepted by RESTORE
    return bytes(body)


class RestoreDuplicateHNSWID(TestCase):
    def getname(self):
        return "RESTORE rejects duplicate HNSW node IDs"

    def test(self):
        seed_key = f"{self.test_key}:seed"
        bad_key = f"{self.test_key}:bad"
        self.redis.delete(seed_key, bad_key)

        # Seed a real Vector Set so we can learn the module id from its DUMP.
        self.redis.execute_command(
            'VADD', seed_key, 'VALUES', '2', '1', '0', 'seed', 'NOQUANT', 'M', '16')

        seed_dump = self.redis.execute_command('DUMP', seed_key)
        assert seed_dump and seed_dump[0] == 7, "unexpected DUMP payload for Vector Set"
        module_id, _ = load_len(seed_dump, 1)
        # Reuse the server's own RDB version from the seed DUMP footer so the
        # crafted payload always clears the version check and reaches the
        # module loader (the code path under test), regardless of the current
        # RDB version.
        rdb_version = struct.unpack("<H", seed_dump[-10:-8])[0]

        # Positive control: a genuine Vector Set must still round-trip.
        self.redis.execute_command('RESTORE', bad_key, '0', seed_dump, 'REPLACE')
        assert self.redis.execute_command('VCARD', bad_key) == 1, \
            "valid Vector Set failed to RESTORE"
        self.redis.delete(bad_key)

        # The crafted payload with duplicate HNSW IDs must be rejected.
        payload = build_dup_id_payload(module_id, rdb_version)
        rejected = False
        try:
            self.redis.execute_command('RESTORE', bad_key, '0', payload, 'REPLACE')
        except redis.exceptions.ResponseError:
            rejected = True
        assert rejected, "RESTORE accepted a Vector Set with duplicate HNSW node IDs"

        # The server must still be alive and functional (no use-after-free).
        assert self.redis.execute_command('PING'), "server did not survive the payload"
        assert self.redis.execute_command('EXISTS', bad_key) == 0, \
            "rejected payload must not create the key"
        self.redis.execute_command(
            'VADD', bad_key, 'VALUES', '2', '1', '0', 'ok', 'NOQUANT', 'M', '16')
        assert self.redis.execute_command('VCARD', bad_key) == 1

        self.redis.delete(seed_key, bad_key)
