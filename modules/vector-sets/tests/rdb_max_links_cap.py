from test import TestCase, generate_random_vector
from tests.rdb_utils import decode_dump, encode_dump
import redis.exceptions

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
