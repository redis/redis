from test import TestCase, generate_random_vector
from tests.rdb_utils import decode_dump, encode_dump
import redis.exceptions

# Mirror of HNSW_MAX_SERIALIZED_NODE_PARAMS in modules/vector-sets/hnsw.h.
HNSW_MAX_M = 4096
HNSW_MAX_LEVEL = 16
HNSW_MAX_SERIALIZED_NODE_PARAMS = \
    2 + (HNSW_MAX_LEVEL + 1) * (2 + HNSW_MAX_M * 4 + 1) + 1

class RdbParamsCountCap(TestCase):
    def getname(self):
        return "[regression] RESTORE rejects oversized node params count"

    def estimated_runtime(self):
        return 4

    def test(self):
        dim = 4
        for i in range(8):
            vec = generate_random_vector(dim)
            self.redis.execute_command('VADD', self.test_key, 'VALUES', dim,
                                       *vec, f'item:{i}')
        module_id, tokens, rdbver = decode_dump(
            self.redis.execute_command('DUMP', self.test_key))

        # Header is dim, count, config, flags (no projection, no attributes),
        # then each node is: item, vector, params_count, params.
        assert tokens[3] == ('u', 0), "unexpected save flags"
        assert [t[0] for t in tokens[4:7]] == ['s', 's', 'u']
        count = tokens[6][1]
        params = tokens[7:7 + count]

        def restore_first_node(declared_count, node_params):
            tampered = tokens[:6] + [('u', declared_count)] + node_params + \
                       tokens[7 + count:]
            self.redis.delete(self.test_key)
            self.redis.execute_command('RESTORE', self.test_key, 0,
                                       encode_dump(module_id, tampered, rdbver))

        def assert_rejected(declared_count, node_params):
            try:
                restore_first_node(declared_count, node_params)
                assert False, f"RESTORE should reject params count {declared_count}"
            except redis.exceptions.ResponseError as e:
                assert "Bad data format" in str(e), f"Unexpected error: {e}"

        # Trailing params are ignored on load, so padding up to the limit
        # must still load.
        limit = HNSW_MAX_SERIALIZED_NODE_PARAMS
        padding = [('u', 0)] * (limit - count)
        restore_first_node(limit, params + padding)
        assert self.redis.execute_command('VCARD', self.test_key) == 8

        assert_rejected(limit + 1, params + padding + [('u', 0)])
        assert_rejected(2**32 + count, params)
        assert_rejected(2**64 - 1, [])

        assert self.redis.ping()
