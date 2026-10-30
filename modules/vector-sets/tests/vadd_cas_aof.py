from test import TestCase
import time

class VADDCASAOFLoad(TestCase):
    def getname(self):
        return "[regression] VADD CAS is replayed synchronously on AOF load"

    def estimated_runtime(self):
        return 0.2

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
        config_name = 'vset-force-single-threaded-execution'
        original_threading = self.redis.config_get(config_name)[config_name]
        original_appendonly = self.redis.config_get('appendonly')['appendonly']
        try:
            self.redis.config_set(config_name, 'no')
            self.redis.config_set('appendonly', 'yes')
            self._wait_rewrite_done()

            # The second VADD goes to an existing set, so it blocks the
            # client and is written to the AOF with CAS.
            self.redis.execute_command('VADD', self.test_key,
                                       'VALUES', 3, 0.1, 0.2, 0.3, 'e1', 'CAS')
            self.redis.execute_command('VADD', self.test_key,
                                       'VALUES', 3, 0.2, 0.2, 0.3, 'e2', 'CAS')

            self.redis.execute_command('DEBUG', 'LOADAOF')

            assert self.redis.ping(), "Server should still be responsive"
            card = self.redis.execute_command('VCARD', self.test_key)
            assert card == 2, f"Expected 2 elements after AOF load, got {card}"
        finally:
            self.redis.delete(self.test_key)
            self.redis.config_set('appendonly', original_appendonly)
            self.redis.config_set(config_name, original_threading)
