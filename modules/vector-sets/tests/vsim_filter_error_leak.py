from test import TestCase
import redis as redis_module

class VSIMFilterLeakOnOptionError(TestCase):
    def getname(self):
        return "[regression] VSIM FILTER expr freed on option parse error"

    def test(self):
        self.redis.execute_command('VADD', self.test_key, 'VALUES', 3, 1, 0, 0, 'elem1')

        # Valid FILTER followed by invalid option values. Before the fix,
        # error paths freed vec but not filter_expr, leaking the compiled
        # exprstate. Under ASAN/valgrind this shows up at server exit.
        error_cmds = [
            # invalid COUNT (0)
            ['VSIM', self.test_key, 'VALUES', 3, 0, 0, 0, 'FILTER', '.a > 0', 'COUNT', 0],
            # invalid EF (0)
            ['VSIM', self.test_key, 'VALUES', 3, 0, 0, 0, 'FILTER', '.a > 0', 'EF', 0],
            # invalid EPSILON (0)
            ['VSIM', self.test_key, 'VALUES', 3, 0, 0, 0, 'FILTER', '.a > 0', 'EPSILON', 0],
            # invalid FILTER-EF (0)
            ['VSIM', self.test_key, 'VALUES', 3, 0, 0, 0, 'FILTER', '.a > 0', 'FILTER-EF', 0],
            # unknown option
            ['VSIM', self.test_key, 'VALUES', 3, 0, 0, 0, 'FILTER', '.a > 0', 'BADOPT', 1],
        ]

        for cmd in error_cmds:
            for _ in range(20):
                try:
                    self.redis.execute_command(*cmd)
                except redis_module.exceptions.ResponseError:
                    pass
