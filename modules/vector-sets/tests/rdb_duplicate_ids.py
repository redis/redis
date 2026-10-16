from test import TestCase, generate_random_vector
from tests.rdb_utils import decode_dump, encode_dump, write_len
import redis.exceptions
import time

class RdbDuplicateIds(TestCase):
    def getname(self):
        return "[regression] RESTORE rejects duplicated node IDs quickly"

    def estimated_runtime(self):
        return 1

    def test(self):
        dim = 4
        vec = generate_random_vector(dim)
        self.redis.execute_command('VADD', self.test_key, 'VALUES', dim,
                                   *vec, 'item:0')
        module_id, tokens, rdbver = decode_dump(
            self.redis.execute_command('DUMP', self.test_key))

        # Header is dim, count, config, flags (no projection, no attributes),
        # then a single node: item, vector, params_count, params.
        assert tokens[3] == ('u', 0), "unexpected save flags"
        header, node = tokens[:4], tokens[4:]
        assert [t[0] for t in node[:3]] == ['s', 's', 'u']
        assert len(node) == 3 + node[2][1]

        # Clone the node many times under distinct element names but with
        # the same serialized ID.
        count = 100000
        tampered = [header[0], ('u', count)] + header[2:]
        for i in range(count):
            name = f'item:{i}'.encode()
            tampered.append(('s', write_len(len(name)) + name))
            tampered.extend(node[1:])

        payload = encode_dump(module_id, tampered, rdbver)
        self.redis.delete(self.test_key)
        start = time.time()
        try:
            self.redis.execute_command('RESTORE', self.test_key, 0, payload)
            assert False, "RESTORE should reject duplicated node IDs"
        except redis.exceptions.ResponseError as e:
            assert "Bad data format" in str(e), f"Unexpected error: {e}"
        elapsed = time.time() - start
        assert elapsed < 5, f"RESTORE took {elapsed:.1f}s to reject the payload"
