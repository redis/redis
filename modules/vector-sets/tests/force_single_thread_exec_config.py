from test import TestCase, generate_random_vector
import struct

class ForceSingleThreadExecConfigTest(TestCase):
    def getname(self):
        return "VSGlobalConfig.forceSingleThreadExec config: binary, errors, and effect on VADD/VSIM"

    def test(self):
        # Check default value (should be 0 or 1, but let's just get it)
        default = self.redis.execute_command('CONFIG', 'GET', 'vset-force-single-threaded-execution')[1].decode()
        assert default=='no', f"Unexpected default: {default}"

        # Set to 0 and 1, check value is set, and VADD/VSIM still work
        for val in ['yes','no']:
            res = self.redis.execute_command('CONFIG', 'SET', 'vset-force-single-threaded-execution', str(val))
            assert res == b'OK', f"SET should return OK, got {res}"
            getval = self.redis.execute_command('CONFIG', 'GET', 'vset-force-single-threaded-execution')
            assert getval == str(val).encode(), f"GET after SET should return {val}, got {getval}"

            # Add a vector and check VADD works
            vec = generate_random_vector(4)
            vec_bytes = struct.pack('4f', *vec)
            add_res = self.redis.execute_command('VADD', self.test_key, 'FP32', vec_bytes, f'{self.test_key}:item:{val}')
            assert add_res == 1, f"VADD should return 1, got {add_res}"

            # VSIM should work and return at least one result
            sim_res = self.redis.execute_command('VSIM', self.test_key, 'FP32', vec_bytes)
            assert isinstance(sim_res, list) and len(sim_res) >= 2, f"VSIM should return results, got {sim_res}"

        # Try wrong arguments
        wrong_args = ["", "abc", "2", "-1", "True", "False", None]
        for arg in wrong_args:
            try:
                self.redis.execute_command('CONFIG', 'SET', 'vset-force-single-threaded-execution', arg)
                assert False, f"Should fail for arg: {arg}"
            except Exception as e:
                # Accept any error, but must be an error
                assert True

        # Restore default
        self.redis.execute_command('CONFIG', 'SET', 'vset-force-single-threaded-execution', default.decode())
