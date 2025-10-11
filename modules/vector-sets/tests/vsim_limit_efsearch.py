from test import TestCase, generate_random_vector
import struct

class VSIMLimitEFSearch(TestCase):
    def getname(self):
        return "VSIM Limit EF Search"

    def estimated_runtime(self):
        return 0.2

    def test(self):
        dim = 32
        vec = generate_random_vector(dim)
        vec_bytes = struct.pack(f'{dim}f', *vec)

        # Add test vector
        self.redis.execute_command('VADD', self.test_key, 'FP32', vec_bytes, f'{self.test_key}:item:1')

        query_vec = generate_random_vector(dim)

        # Test EF upper bound (should accept 1000000)
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', dim,
                                          *[str(x) for x in query_vec], 'EF', 1000000)
        assert isinstance(result, list), "EF=1000000 should be accepted"

        # Test EF over limit (should reject > 1000000)
        try:
            self.redis.execute_command('VSIM', self.test_key, 'VALUES', dim,
                                     *[str(x) for x in query_vec], 'EF', 1000001)
            assert False, "EF=1000001 should be rejected"
        except Exception as e:
            assert "invalid EF" in str(e), f"Expected EF validation error, got: {e}"
