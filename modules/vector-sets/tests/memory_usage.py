from test import TestCase, generate_random_vector
import struct

class MemoryUsageTest(TestCase):
    def getname(self):
        return "[regression] MEMORY USAGE with attributes"

    def test(self):
        # Generate random vectors
        vec1 = generate_random_vector(4)
        vec2 = generate_random_vector(4)
        vec_bytes1 = struct.pack('4f', *vec1)
        vec_bytes2 = struct.pack('4f', *vec2)

        # Add vectors to the key, one with attribute, one without
        self.redis.execute_command('VADD', self.test_key, 'FP32', vec_bytes1, f'{self.test_key}:item:1')
        self.redis.execute_command('VADD', self.test_key, 'FP32', vec_bytes2, f'{self.test_key}:item:2', 'SETATTR', '{"color":"red"}')

        # Get memory usage for the key
        try:
            memory_usage = self.redis.execute_command('MEMORY', 'USAGE', self.test_key)
            # If we got here without exception, the command worked
            assert memory_usage > 0, "MEMORY USAGE should return a positive value"

            # Add more attributes to increase complexity
            self.redis.execute_command('VSETATTR', self.test_key, f'{self.test_key}:item:1', '{"color":"blue","size":10}')

            # Check memory usage again
            new_memory_usage = self.redis.execute_command('MEMORY', 'USAGE', self.test_key)
            assert new_memory_usage > 0, "MEMORY USAGE should still return a positive value after setting attributes"

            # Memory usage should be higher after adding attributes
            assert new_memory_usage > memory_usage, "Memory usage increase after adding attributes"

        except Exception as e:
            raise AssertionError(f"MEMORY USAGE command failed: {str(e)}")
