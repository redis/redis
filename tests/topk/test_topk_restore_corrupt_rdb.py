import struct

from support import *
from rdb_corruption_utils import rewrite_largest_module_string


def _murmur2(data: bytes, seed: int) -> int:
    # Port of MurmurHash2 (32-bit) from deps/murmur2/MurmurHash2.c, used by TopK.
    m = 0x5BD1E995
    r = 24
    length = len(data)
    h = (seed ^ length) & 0xFFFFFFFF
    i = 0
    while length >= 4:
        k = int.from_bytes(data[i : i + 4], "little")
        k = (k * m) & 0xFFFFFFFF
        k ^= k >> r
        k = (k * m) & 0xFFFFFFFF
        h = (h * m) & 0xFFFFFFFF
        h ^= k
        i += 4
        length -= 4
    if length == 3:
        h ^= data[i + 2] << 16
    if length >= 2:
        h ^= data[i + 1] << 8
    if length >= 1:
        h ^= data[i]
        h = (h * m) & 0xFFFFFFFF
    h ^= h >> 13
    h = (h * m) & 0xFFFFFFFF
    h ^= h >> 15
    return h & 0xFFFFFFFF


def _corrupt_dump_patch_largest_module_string(dump_payload: bytes, patch_fn) -> bytes:
    # Decode the largest MODULE_OPCODE_STRING, apply patch_fn(decoded)->bytes (same
    # length), re-emit it uncompressed, and fix up the trailing CRC64. Keeping the
    # string length intact means RDB-load size validations still pass and the
    # patched *contents* are exercised.
    return rewrite_largest_module_string(dump_payload, patch_fn, require_same_length=True)


class testTopKRestoreCorruptRDB():
    def __init__(self):
        # We need raw bytes for DUMP/RESTORE payload manipulation
        self.env = Env(decodeResponses=False)

    def test_restore_topk_inflated_itemlen_is_safe(self):
        # Robustness test: a restored TopK whose heap blob carries an `itemlen`
        # that does not match the stored item buffer must still behave correctly.
        # TopKRdbLoad only validates the total heap blob size, so a payload with a
        # valid size but a larger-than-real `itemlen` would otherwise leave the
        # bucket length out of sync with its item buffer. The fix derives `itemlen`
        # from the loaded buffer, so item comparisons during queries stay
        # consistent. Here we load such a payload and confirm TOPK queries keep
        # working and the server stays healthy.
        env = self.env
        env.cmd("FLUSHALL")

        key = b"topk_il{topk}"
        corrupt_key = b"topk_il_corrupt{topk}"

        # k=1,width=1,depth=1 -> the heap blob (one HeapBucket) is the largest
        # module string, so the patch helper targets it.
        env.cmd("TOPK.RESERVE", key, 1, 1, 1, 0.9)
        env.cmd("TOPK.ADD", key, b"a")  # heap[0].item -> "a" (2-byte allocation)

        dump_payload = env.cmd("DUMP", key)

        GA = 1919  # fingerprint seed used by TopK (see topk.c)
        probe = b"A" * 200  # query whose length we set as the bucket's itemlen
        probe_fp = _murmur2(probe, GA)

        def patch_heap(decoded: bytes) -> bytes:
            # HeapBucket layout: uint32 fp; uint32 itemlen; char *item; uint32 count
            b = bytearray(decoded)
            b[0:4] = struct.pack("<I", probe_fp)     # fp matches our probe
            b[4:8] = struct.pack("<I", len(probe))   # itemlen larger than the stored item
            return bytes(b)

        corrupted = _corrupt_dump_patch_largest_module_string(dump_payload, patch_heap)

        # The heap blob size is valid, so the payload loads; the fix realigns
        # `itemlen` with the stored item buffer.
        env.cmd("RESTORE", corrupt_key, 0, corrupted)

        # These queries compare the probe against the (short) stored item; with the
        # fix they complete normally and the server stays healthy.
        env.cmd("TOPK.QUERY", corrupt_key, probe)
        env.cmd("TOPK.COUNT", corrupt_key, probe)
        env.cmd("TOPK.ADD", corrupt_key, probe)
        env.assertEqual(env.cmd("PING"), True)
