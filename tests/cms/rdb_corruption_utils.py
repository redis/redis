import struct


# RDB module opcodes / encodings (see Redis RDB format)
RDB_6BITLEN = 0
RDB_14BITLEN = 1
RDB_ENC_INT8 = 0
RDB_ENC_INT16 = 1
RDB_ENC_LZF = 3
RDB_MODULE_OPCODE_EOF = 0
RDB_MODULE_OPCODE_UINT = 2
RDB_MODULE_OPCODE_DOUBLE = 4
RDB_MODULE_OPCODE_STRING = 5

CRC64_POLY = 0xAD93D23594C935A9


def _crc_reflect(data: int, bits: int) -> int:
    out = 0
    for i in range(bits):
        if data & (1 << i):
            out |= 1 << (bits - 1 - i)
    return out


def crc64_redis(data: bytes) -> int:
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


def encode_len(n: int) -> bytes:
    if n < (1 << 6):
        return bytes([n & 0x3F])
    if n < (1 << 14):
        return bytes([0x40 | ((n >> 8) & 0x3F), n & 0xFF])
    if n < (1 << 32):
        return bytes([0x80, (n >> 24) & 0xFF, (n >> 16) & 0xFF, (n >> 8) & 0xFF, n & 0xFF])
    return bytes([0x81]) + n.to_bytes(8, "big")


def load_len(buf: bytes, pos: int):
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


def lzf_decompress(data: bytes, out_len: int) -> bytes:
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


def rewrite_largest_module_string(dump_payload: bytes, transform_fn, require_same_length=False) -> bytes:
    """Rewrite the largest module string and update the DUMP payload CRC."""
    if len(dump_payload) < 10:
        raise RuntimeError("DUMP payload too small")
    value = dump_payload[:-10]
    version = dump_payload[-10:-8]

    pos = 1
    _, is_enc, pos, _ = load_len(value, pos)  # module id
    if is_enc:
        raise RuntimeError("Unexpected encoded module-id length")

    strings = []
    while pos < len(value):
        opcode, is_enc, pos, _ = load_len(value, pos)
        if is_enc:
            raise RuntimeError(f"Unexpected encoded opcode at pos={pos}")
        if opcode == RDB_MODULE_OPCODE_EOF:
            break
        if opcode == RDB_MODULE_OPCODE_UINT:
            _, is_enc2, pos, _ = load_len(value, pos)
            if is_enc2:
                raise RuntimeError("Unexpected encoded value in MODULE_OPCODE_UINT")
            continue
        if opcode == RDB_MODULE_OPCODE_DOUBLE:
            pos += 8
            continue
        if opcode == RDB_MODULE_OPCODE_STRING:
            len_start = pos
            slen_or_enc, is_str_enc, pos, enc = load_len(value, pos)
            if not is_str_enc:
                data_end = pos + slen_or_enc
                if data_end > len(value):
                    raise RuntimeError("String overruns buffer while parsing")
                decoded = value[pos:data_end]
                pos = data_end
            else:
                if enc != RDB_ENC_LZF:
                    raise RuntimeError(f"Unsupported encoded string type: {enc}")
                compressed_len, is_enc3, pos, _ = load_len(value, pos)
                ulen, is_enc4, pos, _ = load_len(value, pos)
                if is_enc3 or is_enc4:
                    raise RuntimeError("Unexpected encoded compressed/uncompressed length")
                data_end = pos + compressed_len
                if data_end > len(value):
                    raise RuntimeError("Compressed string overruns buffer while parsing")
                decoded = lzf_decompress(value[pos:data_end], ulen)
                pos = data_end
            strings.append({"len_start": len_start, "old_end": pos, "decoded": decoded})
            continue
        raise RuntimeError(f"Unknown module opcode {opcode} at pos={pos}")

    if not strings:
        raise RuntimeError("No MODULE_OPCODE_STRING entries found; cannot corrupt payload")

    target = max(strings, key=lambda entry: len(entry["decoded"]))
    new_data = transform_fn(target["decoded"])
    if require_same_length and len(new_data) != len(target["decoded"]):
        raise RuntimeError("transform_fn must preserve the string length")

    new_value = (
        value[: target["len_start"]]
        + encode_len(len(new_data))
        + new_data
        + value[target["old_end"] :]
    )
    new_crc = crc64_redis(new_value + version)
    return new_value + version + struct.pack("<Q", new_crc)


def rewrite_module_uint(dump_payload: bytes, index: int, new_value: int) -> bytes:
    """Rewrite the index-th MODULE_OPCODE_UINT field and update the DUMP payload CRC.

    Lets a test build a payload whose scalar fields disagree with the rest of the
    value - a state no sequence of commands can reach, but a crafted RESTORE can.
    """
    if len(dump_payload) < 10:
        raise RuntimeError("DUMP payload too small")
    value = dump_payload[:-10]
    version = dump_payload[-10:-8]

    pos = 1
    _, is_enc, pos, _ = load_len(value, pos)  # module id
    if is_enc:
        raise RuntimeError("Unexpected encoded module-id length")

    out = bytearray(value[:pos])
    seen = 0
    rewritten = False
    while pos < len(value):
        op_start = pos
        opcode, is_enc, pos, _ = load_len(value, pos)
        if is_enc:
            raise RuntimeError(f"Unexpected encoded opcode at pos={pos}")
        after_opcode = pos

        if opcode == RDB_MODULE_OPCODE_EOF:
            out += value[op_start:]
            break

        if opcode == RDB_MODULE_OPCODE_UINT:
            _, is_enc2, pos, _ = load_len(value, pos)
            if is_enc2:
                raise RuntimeError("Unexpected encoded value in MODULE_OPCODE_UINT")
            if seen == index:
                out += value[op_start:after_opcode] + encode_len(new_value)
                rewritten = True
            else:
                out += value[op_start:pos]
            seen += 1
            continue

        if opcode == RDB_MODULE_OPCODE_DOUBLE:
            pos += 8
            out += value[op_start:pos]
            continue

        if opcode == RDB_MODULE_OPCODE_STRING:
            slen_or_enc, is_str_enc, pos, enc = load_len(value, pos)
            if not is_str_enc:
                pos += slen_or_enc
            else:
                if enc != RDB_ENC_LZF:
                    raise RuntimeError(f"Unsupported encoded string type: {enc}")
                compressed_len, is_enc3, pos, _ = load_len(value, pos)
                _, is_enc4, pos, _ = load_len(value, pos)  # uncompressed length
                if is_enc3 or is_enc4:
                    raise RuntimeError("Unexpected encoded compressed/uncompressed length")
                pos += compressed_len
            if pos > len(value):
                raise RuntimeError("String overruns buffer while parsing")
            out += value[op_start:pos]
            continue

        raise RuntimeError(f"Unknown module opcode {opcode} at pos={pos}")

    if not rewritten:
        raise RuntimeError(f"No MODULE_OPCODE_UINT at index {index}")

    new_body = bytes(out)
    new_crc = crc64_redis(new_body + version)
    return new_body + version + struct.pack("<Q", new_crc)
