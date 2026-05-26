from test import TestCase, generate_random_vector
import struct


class VAddCASSetAttrAccounting(TestCase):
    def getname(self):
        return "[regression] VADD CAS SETATTR keeps attribute accounting"

    def estimated_runtime(self):
        return 0.1

    def _vinfo_map(self):
        info = self.redis.execute_command('VINFO', self.test_key)
        return {
            (k.decode() if isinstance(k, bytes) else k): v
            for k, v in zip(info[::2], info[1::2])
        }

    def _decode_attr(self, value):
        return value.decode() if isinstance(value, bytes) else value

    def test(self):
        self.redis.execute_command(
            'CONFIG', 'SET', 'vset-force-single-threaded-execution', 'no')

        dim = 4
        seed_vec = generate_random_vector(dim)
        seed_vec_bytes = struct.pack(f'{dim}f', *seed_vec)
        result = self.redis.execute_command(
            'VADD', self.test_key, 'FP32', seed_vec_bytes,
            f'{self.test_key}:seed')
        assert result == 1, f"Seed VADD should return 1, got {result}"

        cas_vec = generate_random_vector(dim)
        cas_vec_bytes = struct.pack(f'{dim}f', *cas_vec)
        attr = '{"a":1}'
        item = f'{self.test_key}:item:cas'
        result = self.redis.execute_command(
            'VADD', self.test_key, 'FP32', cas_vec_bytes, item,
            'CAS', 'SETATTR', attr)
        assert result == 1, f"CAS VADD should return 1, got {result}"

        stored_attr = self.redis.execute_command('VGETATTR', self.test_key, item)
        assert self._decode_attr(stored_attr) == attr, (
            f"Expected attribute {attr}, got {stored_attr}")

        info_map = self._vinfo_map()
        assert info_map['attributes-count'] == 1, (
            "VINFO should report one attribute after VADD ... CAS SETATTR")

        self.redis.execute_command('DEBUG', 'RELOAD')

        reloaded_attr = self.redis.execute_command('VGETATTR', self.test_key, item)
        assert self._decode_attr(reloaded_attr) == attr, (
            f"Expected attribute {attr} after reload, got {reloaded_attr}")

        reloaded_info = self._vinfo_map()
        assert reloaded_info['attributes-count'] == 1, (
            "VINFO should still report one attribute after reload")
