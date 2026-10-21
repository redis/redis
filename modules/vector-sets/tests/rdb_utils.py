import struct

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
