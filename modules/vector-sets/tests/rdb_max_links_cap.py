from test import TestCase, generate_random_vector
import struct
import redis.exceptions

RDB_TYPE_MODULE_2 = 7
OPCODE_EOF, OPCODE_UINT, OPCODE_STRING = 0, 2, 5

def crc64(data):
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0x95ac9329ac4bc9b5 if crc & 1 else 0)
    return crc

def read_len(buf, pos):
    """Returns (value, is_encoded, new_pos)."""
    b = buf[pos]
    kind = b >> 6
    if kind == 0: return b & 0x3f, False, pos + 1
    if kind == 1: return ((b & 0x3f) << 8) | buf[pos + 1], False, pos + 2
    if kind == 3: return b & 0x3f, True, pos + 1
    if b == 0x80: return struct.unpack('>I', buf[pos+1:pos+5])[0], False, pos + 5
    if b == 0x81: return struct.unpack('>Q', buf[pos+1:pos+9])[0], False, pos + 9
    raise ValueError(f"bad length byte {b:#x}")

def write_len(v):
    if v < 1 << 6: return bytes([v])
    if v < 1 << 14: return bytes([0x40 | (v >> 8), v & 0xff])
    if v < 1 << 32: return b'\x80' + struct.pack('>I', v)
    return b'\x81' + struct.pack('>Q', v)

def skip_string(buf, pos):
    v, encoded, pos = read_len(buf, pos)
    if not encoded: return pos + v
    if v < 3: return pos + (1 << v)   # INT8 / INT16 / INT32
    compressed_len, _, pos = read_len(buf, pos)  # LZF
    _, _, pos = read_len(buf, pos)
    return pos + compressed_len

def decode_dump(payload):
    """Split a vector set DUMP payload into (module_id, tokens, rdb_version),
    where tokens are ('u', int) or ('s', raw_encoded_bytes)."""
    body, rdbver = payload[:-10], payload[-10:-8]
    assert body[0] == RDB_TYPE_MODULE_2
    module_id, _, pos = read_len(body, 1)
    tokens = []
    while True:
        op, _, pos = read_len(body, pos)
        if op == OPCODE_EOF: break
        if op == OPCODE_UINT:
            v, _, pos = read_len(body, pos)
            tokens.append(('u', v))
        elif op == OPCODE_STRING:
            end = skip_string(body, pos)
            tokens.append(('s', body[pos:end]))
            pos = end
        else:
            raise ValueError(f"unexpected opcode {op}")
    return module_id, tokens, rdbver

def encode_dump(module_id, tokens, rdbver):
    out = bytes([RDB_TYPE_MODULE_2]) + write_len(module_id)
    for kind, v in tokens:
        if kind == 'u': out += write_len(OPCODE_UINT) + write_len(v)
        else: out += write_len(OPCODE_STRING) + v
    out += write_len(OPCODE_EOF) + rdbver
    return out + struct.pack('<Q', crc64(out))

class RdbMaxLinksCap(TestCase):
    def getname(self):
        return "[regression] RESTORE rejects nodes with oversized max links"

    def estimated_runtime(self):
        return 0.2

    def test(self):
        dim, m = 4, 4
        for i in range(64):
            vec = generate_random_vector(dim)
            self.redis.execute_command('VADD', self.test_key, 'VALUES', dim,
                                       *vec, f'item:{i}', 'M', m)
        module_id, tokens, rdbver = decode_dump(
            self.redis.execute_command('DUMP', self.test_key))

        # Header is dim, count, config, flags (no projection, no attributes),
        # then each node is: item, vector, params_count, params.
        assert tokens[3] == ('u', 0), "unexpected save flags"
        assert [t[0] for t in tokens[4:7]] == ['s', 's', 'u']

        def max_links_index(layer):
            """Token index of max_links at `layer` for the first node that
            reaches it. Params are id, level, then per layer: num_links,
            max_links, links, worst_idx."""
            pos = 4
            while pos < len(tokens):
                params_count = tokens[pos + 2][1]
                params = pos + 3
                if tokens[params + 1][1] & 0xff >= layer:
                    idx = params + 2
                    for _ in range(layer):
                        idx += 3 + tokens[idx][1]
                    return idx + 1
                pos = params + params_count
            assert False, f"no node reaches layer {layer}"

        def restore_with_max_links(idx, max_links):
            tampered = list(tokens)
            tampered[idx] = ('u', max_links)
            self.redis.delete(self.test_key)
            self.redis.execute_command('RESTORE', self.test_key, 0,
                                       encode_dump(module_id, tampered, rdbver))

        for layer, limit in ((0, m * 3), (1, m * 2)):
            idx = max_links_index(layer)

            # The largest capacity reachable at runtime must still load.
            restore_with_max_links(idx, limit)
            assert self.redis.execute_command('VCARD', self.test_key) == 64

            try:
                restore_with_max_links(idx, limit + 1)
                assert False, f"RESTORE should reject layer {layer} max links beyond the M bound"
            except redis.exceptions.ResponseError as e:
                assert "Bad data format" in str(e), f"Unexpected error: {e}"
