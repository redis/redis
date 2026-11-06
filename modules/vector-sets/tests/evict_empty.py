from test import TestCase, generate_random_vector
import struct

class VREM_LastItemDeletesKey(TestCase):
    def getname(self):
        return "VREM last item deletes key"

    def test(self):
        # Generate a random vector
        vec = generate_random_vector(4)
        vec_bytes = struct.pack('4f', *vec)

        # Add the vector to the key
        result = self.redis.execute_command('VADD', self.test_key, 'FP32', vec_bytes, f'{self.test_key}:item:1')
        assert result == 1, "VADD should return 1 for first item"

        # Verify the key exists
        exists = self.redis.exists(self.test_key)
        assert exists == 1, "Key should exist after VADD"

        # Remove the item
        result = self.redis.execute_command('VREM', self.test_key, f'{self.test_key}:item:1')
        assert result == 1, "VREM should return 1 for successful removal"

        # Verify the key no longer exists
        exists = self.redis.exists(self.test_key)
        assert exists == 0, "Key should no longer exist after VREM of last item"
