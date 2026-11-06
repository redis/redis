from test import TestCase
import time

class AOFRewriteWithoutCallback(TestCase):
    def getname(self):
        return "AOF rewrite of a vector set without aof_rewrite fails gracefully"

    def _wait_rewrite_done(self, timeout=30):
        deadline = time.time() + timeout
        while time.time() < deadline:
            info = self.redis.info('persistence')
            if (info.get('aof_rewrite_in_progress') == 0 and
                info.get('aof_rewrite_scheduled') == 0):
                return info
            time.sleep(0.1)
        raise AssertionError("AOF rewrite did not complete in time")

    def test(self):
        # Vector Sets register their module data type with a NULL aof_rewrite
        # callback. A pure AOF rewrite (no RDB preamble) used to dereference
        # that NULL pointer and crash the rewrite child process (#15387).
        original_appendonly = self.redis.config_get('appendonly')['appendonly']
        original_preamble = \
            self.redis.config_get('aof-use-rdb-preamble')['aof-use-rdb-preamble']
        try:
            # Enable AOF with the RDB preamble still on, so the initial
            # rewrite triggered by enabling it succeeds.
            self.redis.config_set('appendonly', 'yes')
            info = self._wait_rewrite_done()
            assert info['aof_last_bgrewrite_status'] == 'ok', \
                "Initial AOF rewrite with RDB preamble should succeed"

            self.redis.config_set('aof-use-rdb-preamble', 'no')
            self.redis.execute_command('VADD', self.test_key,
                                       'VALUES', 3, 1, 2, 3, 'elem1')

            self.redis.execute_command('BGREWRITEAOF')
            info = self._wait_rewrite_done()

            # The rewrite must fail with an error instead of crashing the
            # rewrite child, and the server must stay fully functional.
            assert info['aof_last_bgrewrite_status'] == 'err', \
                "AOF rewrite of a type without aof_rewrite should fail"
            assert self.redis.ping(), "Server should still be responsive"
            assert self.redis.execute_command('TYPE', self.test_key) == \
                b'vectorset', "Vector set should still be intact"
        finally:
            self.redis.delete(self.test_key)
            self.redis.config_set('aof-use-rdb-preamble', original_preamble)
            self.redis.config_set('appendonly', original_appendonly)
