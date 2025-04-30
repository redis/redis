from test import TestCase, generate_random_vector
import struct

class BasicVISMEMBER(TestCase):
    def getname(self):
        return "VISMEMBER basic functionality"

    def test(self):
        # Add multiple vectors to the vector set
        vec1 = generate_random_vector(4)
        vec2 = generate_random_vector(4)
        vec_bytes1 = struct.pack('4f', *vec1)
        vec_bytes2 = struct.pack('4f', *vec2)

        # Create item keys
        item1 = f'{self.test_key}:item:1'
        item2 = f'{self.test_key}:item:2'
        nonexistent_item = f'{self.test_key}:item:nonexistent'

        # Add the vectors
        self.redis.execute_command('VADD', self.test_key, 'FP32', vec_bytes1, item1)
        self.redis.execute_command('VADD', self.test_key, 'FP32', vec_bytes2, item2)

        # Test VISMEMBER with existing elements
        result1 = self.redis.execute_command('VISMEMBER', self.test_key, item1)
        assert result1 == 1, f"VISMEMBER should return 1 for existing item, got {result1}"

        result2 = self.redis.execute_command('VISMEMBER', self.test_key, item2)
        assert result2 == 1, f"VISMEMBER should return 1 for existing item, got {result2}"

        # Test VISMEMBER with non-existent element
        result3 = self.redis.execute_command('VISMEMBER', self.test_key, nonexistent_item)
        assert result3 == 0, f"VISMEMBER should return 0 for non-existent item, got {result3}"

        # Test VISMEMBER with non-existent key
        nonexistent_key = f'{self.test_key}_nonexistent'
        result4 = self.redis.execute_command('VISMEMBER', nonexistent_key, item1)
        assert result4 == 0, f"VISMEMBER should return 0 for non-existent key, got {result4}"

        # Test VISMEMBER after removing an element
        self.redis.execute_command('VREM', self.test_key, item1)
        result5 = self.redis.execute_command('VISMEMBER', self.test_key, item1)
        assert result5 == 0, f"VISMEMBER should return 0 after element removal, got {result5}"

        # Verify item2 still exists
        result6 = self.redis.execute_command('VISMEMBER', self.test_key, item2)
        assert result6 == 1, f"VISMEMBER should still return 1 for remaining item, got {result6}"
