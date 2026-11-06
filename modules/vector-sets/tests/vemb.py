from test import TestCase
import struct
import math

class VEMB(TestCase):
    def getname(self):
        return "VEMB Command"

    def test(self):
        dim = 4

        # Add same vector in both formats
        vec = [1, 0, 0, 0]
        norm = math.sqrt(sum(x*x for x in vec))
        vec = [x/norm for x in vec]  # Normalize the vector

        # Add using FP32
        vec_bytes = struct.pack(f'{dim}f', *vec)
        self.redis.execute_command('VADD', self.test_key, 'FP32', vec_bytes, f'{self.test_key}:item:1')

        # Add using VALUES
        self.redis.execute_command('VADD', self.test_key, 'VALUES', dim, 
                         *[str(x) for x in vec], f'{self.test_key}:item:2')

        # Get both back with VEMB
        result1 = self.redis.execute_command('VEMB', self.test_key, f'{self.test_key}:item:1')
        result2 = self.redis.execute_command('VEMB', self.test_key, f'{self.test_key}:item:2')

        retrieved_vec1 = [float(x) for x in result1]
        retrieved_vec2 = [float(x) for x in result2]

        # Compare both vectors with original (allow for small quantization errors)
        for i in range(dim):
            assert abs(vec[i] - retrieved_vec1[i]) < 0.01, \
                f"FP32 vector component {i} mismatch: expected {vec[i]}, got {retrieved_vec1[i]}"
            assert abs(vec[i] - retrieved_vec2[i]) < 0.01, \
                f"VALUES vector component {i} mismatch: expected {vec[i]}, got {retrieved_vec2[i]}"

        # Test non-existent item
        result = self.redis.execute_command('VEMB', self.test_key, 'nonexistent')
        assert result is None, "Non-existent item should return nil"
