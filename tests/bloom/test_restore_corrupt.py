import struct
from common import *

RDB_6BITLEN = 0
RDB_14BITLEN = 1
RDB_ENC_INT8 = 0
RDB_ENC_INT16 = 1
RDB_ENC_INT32 = 2
RDB_ENC_LZF = 3
CRC64_POLY = 0xAD93D23594C935A9
BLOOM_NATIVE_TYPE = 37
BLOOM_LEGACY_TYPE = 7
BLOOM_LEGACY_ID = 0x3016e5a289bef804

def _crc_reflect(data: int, bits: int) -> int:
    out = 0
    for i in range(bits):
        if data & (1 << i):
            out |= 1 << (bits - 1 - i)
    return out


def _crc64_redis(data: bytes) -> int:
    crc = 0
    for b in data:
        for i in range(8):
            bit = crc & 0x8000000000000000
            if b & (1 << i):
                bit = 0 if bit else 1
            crc = (crc << 1) & 0xFFFFFFFFFFFFFFFF
            if bit:
                crc ^= CRC64_POLY
    return _crc_reflect(crc, 64) & 0xFFFFFFFFFFFFFFFF


def _encode_len(n: int) -> bytes:
    if n < (1 << 6):
        return bytes([n & 0x3F])
    if n < (1 << 14):
        return bytes([0x40 | ((n >> 8) & 0x3F), n & 0xFF])
    if n < (1 << 32):
        return bytes([0x80, (n >> 24) & 0xFF, (n >> 16) & 0xFF, (n >> 8) & 0xFF, n & 0xFF])
    return bytes([0x81]) + n.to_bytes(8, "big")


def _load_len(buf: bytes, pos: int):
    first = buf[pos]
    typ = (first & 0xC0) >> 6
    if typ == RDB_6BITLEN:
        return first & 0x3F, False, pos + 1, None
    if typ == RDB_14BITLEN:
        val = ((first & 0x3F) << 8) | buf[pos + 1]
        return val, False, pos + 2, None
    if typ == 2:
        enc = first & 0x3F
        if enc == RDB_ENC_INT8:
            val = int.from_bytes(buf[pos + 1 : pos + 5], "big")
            return val, False, pos + 5, None
        if enc == RDB_ENC_INT16:
            val = int.from_bytes(buf[pos + 1 : pos + 9], "big")
            return val, False, pos + 9, None
        return enc, True, pos + 1, enc
    enc = first & 0x3F
    return enc, True, pos + 1, enc


def _lzf_decompress(data: bytes, out_len: int) -> bytes:
    i = 0
    out = bytearray()
    while i < len(data):
        ctrl = data[i]
        i += 1
        if ctrl < 32:
            length = ctrl + 1
            out.extend(data[i : i + length])
            i += length
        else:
            length = ctrl >> 5
            ref = len(out) - ((ctrl & 0x1F) << 8) - 1
            if length == 7:
                length += data[i]
                i += 1
            ref -= data[i]
            i += 1
            length += 2
            for _ in range(length):
                out.append(out[ref])
                ref += 1
    if len(out) != out_len:
        raise RuntimeError(f"LZF output length mismatch (got {len(out)}, expected {out_len})")
    return bytes(out)



def _fields(payload):
    value = payload[:-10]
    legacy = value[0] == BLOOM_LEGACY_TYPE
    assert legacy or value[0] == BLOOM_NATIVE_TYPE
    pos = 1
    if legacy:
        moduleid, encoded, pos, _ = _load_len(value, pos)
        assert not encoded and moduleid == BLOOM_LEGACY_ID
    fields = []

    def take(kind):
        nonlocal pos
        if legacy:
            opcode, encoded, pos, _ = _load_len(value, pos)
            assert not encoded and opcode == {'uint': 2, 'double': 4, 'string': 5}[kind]
        start = pos
        if kind == 'uint':
            result, encoded, pos, _ = _load_len(value, pos)
            assert not encoded
        elif kind == 'double':
            result = struct.unpack('<d', value[pos:pos + 8])[0]
            pos += 8
        else:
            length, encoded, pos, encoding = _load_len(value, pos)
            if encoded:
                assert encoding == RDB_ENC_LZF
                compressed, _, pos, _ = _load_len(value, pos)
                length, _, pos, _ = _load_len(value, pos)
                result = _lzf_decompress(value[pos:pos + compressed], length)
                pos += compressed
            else:
                result = value[pos:pos + length]
                pos += length
        fields.append((kind, start, pos, result))
        return result

    take('uint')  # size
    count = take('uint')
    take('uint')  # options
    take('uint')  # growth
    for _ in range(count):
        for kind in ('uint', 'double', 'uint', 'double', 'uint', 'uint', 'string', 'uint'):
            take(kind)
    return fields


def _with_checksum(body):
    return body + struct.pack('<Q', _crc64_redis(body))


def _formats(native):
    # Exercise the migrated native loader and backward-compatible module loader.
    legacy = bytes([BLOOM_LEGACY_TYPE]) + _encode_len(BLOOM_LEGACY_ID)
    for kind, start, end, _ in _fields(native):
        legacy += bytes([{'uint': 2, 'double': 4, 'string': 5}[kind]]) + native[start:end]
    legacy += b'\0' + native[-10:-8]
    return native, _with_checksum(legacy)


def _patch(payload, kind, index, value):
    field = [f for f in _fields(payload) if f[0] == kind][index - 1]
    _, start, end, _ = field
    if kind == 'uint':
        encoded = _encode_len(value)
    elif kind == 'double':
        encoded = struct.pack('<d', value)
    else:
        encoded = _encode_len(len(value)) + value
    return _with_checksum(payload[:start] + encoded + payload[end:-8])


class testBFRestoreCorruptRDB:
    def __init__(self):
        self.env = Env(decodeResponses=False)

    def test_restore_rejects_corrupted_bloom_bitset(self):
        env = self.env
        env.cmd('FLUSHALL')
        env.cmd('BF.RESERVE', 'bf', 0.01, 1000)
        for payload in _formats(env.cmd('DUMP', 'bf')):
            # Verify the unmodified fixture before testing rejection.
            env.cmd('RESTORE', 'valid', 0, payload, 'REPLACE')
            data = next(f[3] for f in _fields(payload) if f[0] == 'string')
            corrupt = _patch(payload, 'string', 1, data[:max(1, len(data) // 4)])
            env.assertRaises(ResponseError, env.cmd, 'RESTORE', 'corrupt', 0, corrupt)
            env.assertEqual(env.cmd('EXISTS', 'corrupt'), 0)
        env.cmd('BF.ADD', 'bf', 'sanity')

    def test_restore_rejects_oversized_nfilters(self):
        env = self.env
        env.cmd('FLUSHALL')
        env.cmd('BF.RESERVE', 'bf', 0.01, 1000)
        for payload in _formats(env.cmd('DUMP', 'bf')):
            corrupt = _patch(payload, 'uint', 2, 0xffffffffffffffff)
            env.assertRaises(ResponseError, env.cmd, 'RESTORE', 'corrupt', 0, corrupt)
            env.assertEqual(env.cmd('EXISTS', 'corrupt'), 0)
        env.cmd('BF.ADD', 'bf', 'sanity')
        env.assertEqual(env.cmd('PING'), True)

    def test_restore_rejects_invalid_bloom_numeric_state(self):
        env = self.env
        env.cmd('FLUSHALL')
        env.cmd('BF.RESERVE', 'bf', 0.01, 1000)
        for payload in _formats(env.cmd('DUMP', 'bf')):
            for kind, index, value in (
                ('double', 1, float('nan')), ('double', 2, float('inf')),
                ('uint', 3, 1 << 32), ('uint', 6, (1 << 32) + 7),
                ('uint', 8, (1 << 32) + 14),
            ):
                corrupt = _patch(payload, kind, index, value)
                env.assertRaises(ResponseError, env.cmd, 'RESTORE', 'corrupt', 0, corrupt)
                env.assertEqual(env.cmd('EXISTS', 'corrupt'), 0)
        env.assertEqual(env.cmd('PING'), True)
