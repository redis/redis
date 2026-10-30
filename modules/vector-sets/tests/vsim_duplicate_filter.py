from test import TestCase

class VSIMDuplicateFilterLeak(TestCase):
    def getname(self):
        return "[regression] VSIM duplicate FILTER should not leak memory"

    def test(self):
        self.redis.execute_command('VADD', self.test_key, 'VALUES', 3, 0.5774, 0.5774, 0.5774, 'elem1')
        self.redis.execute_command('VADD', self.test_key, 'VALUES', 3, 0.7071, 0.7071, 0.0, 'elem2')
        self.redis.execute_command('VSETATTR', self.test_key, 'elem1', '{"a": 1, "b": 2}')
        self.redis.execute_command('VSETATTR', self.test_key, 'elem2', '{"a": 2, "b": 3}')

        # Duplicate FILTER: before the fix the first exprstate was
        # overwritten without exprFree(), leaking ~760 bytes per call.
        # Under ASAN/valgrind this shows up as a leak at server exit.
        for _ in range(100):
            self.redis.execute_command(
                'VSIM', self.test_key, 'VALUES', 3, 0.5774, 0.5774, 0.5774,
                'FILTER', '.a == 1', 'FILTER', '.b >= 1')
