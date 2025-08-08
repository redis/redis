from test import TestCase, generate_random_vector
import struct

class BasicCommands(TestCase):
    def getname(self):
        return "VADD, VDIM, VCARD basic usage"

    def test(self):
        # Test VADD
        vec = generate_random_vector(4)
        vec_bytes = struct.pack('4f', *vec)
        result = self.redis.execute_command('VADD', self.test_key, 'FP32', vec_bytes, f'{self.test_key}:item:1')
        assert result == 1, "VADD should return 1 for first item"

        # Test VDIM
        dim = self.redis.execute_command('VDIM', self.test_key)
        assert dim == 4, f"VDIM should return 4, got {dim}"

        # Test VCARD
        card = self.redis.execute_command('VCARD', self.test_key)
        assert card == 1, f"VCARD should return 1, got {card}"
