from test import TestCase, generate_random_vector
import struct

class DebugDigestTest(TestCase):
    def getname(self):
        return "[regression] DEBUG DIGEST-VALUE with attributes"

    def test(self):
        # Generate random vectors
        vec1 = generate_random_vector(4)
        vec2 = generate_random_vector(4)
        vec_bytes1 = struct.pack('4f', *vec1)
        vec_bytes2 = struct.pack('4f', *vec2)

        # Add vectors to the key, one with attribute, one without
        self.redis.execute_command('VADD', self.test_key, 'FP32', vec_bytes1, f'{self.test_key}:item:1')
        self.redis.execute_command('VADD', self.test_key, 'FP32', vec_bytes2, f'{self.test_key}:item:2', 'SETATTR', '{"color":"red"}')

        # Call DEBUG DIGEST-VALUE on the key
        try:
            digest1 = self.redis.execute_command('DEBUG', 'DIGEST-VALUE', self.test_key)
            assert digest1 is not None, "DEBUG DIGEST-VALUE should return a value"

            # Change attribute and verify digest changes
            self.redis.execute_command('VSETATTR', self.test_key, f'{self.test_key}:item:2', '{"color":"blue"}')

            digest2 = self.redis.execute_command('DEBUG', 'DIGEST-VALUE', self.test_key)
            assert digest2 is not None, "DEBUG DIGEST-VALUE should return a value after attribute change"
            assert digest1 != digest2, "Digest should change when an attribute is modified"

            # Remove attribute and verify digest changes again
            self.redis.execute_command('VSETATTR', self.test_key, f'{self.test_key}:item:2', '')

            digest3 = self.redis.execute_command('DEBUG', 'DIGEST-VALUE', self.test_key)
            assert digest3 is not None, "DEBUG DIGEST-VALUE should return a value after attribute removal"
            assert digest2 != digest3, "Digest should change when an attribute is removed"

        except Exception as e:
            raise AssertionError(f"DEBUG DIGEST-VALUE command failed: {str(e)}")
