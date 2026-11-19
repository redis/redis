from test import TestCase
import time


class VAddUpdateReplication(TestCase):
    def getname(self):
        return "[regression] VADD update of an existing element: replication"

    def estimated_runtime(self):
        # Run after the comprehensive replication test (20), which is the
        # first one to attach the replica: the tests before it keep running
        # without a replica, as they did.
        return 21.0

    def test(self):
        assert self.setup_replication(), "Failed to setup replication"

        item = f'{self.test_key}:item'
        result = self.redis.execute_command(
            'VADD', self.test_key, 'VALUES', 3, 1, 0, 0, item)
        assert result == 1, f"First VADD should return 1, got {result}"
        added_emb = self.redis.execute_command('VEMB', self.test_key, item)

        # Same element, another vector: the element is updated.
        result = self.redis.execute_command(
            'VADD', self.test_key, 'VALUES', 3, 0, 1, 0, item)
        assert result == 0, f"VADD of an existing element should return 0, got {result}"
        updated_emb = self.redis.execute_command('VEMB', self.test_key, item)
        assert updated_emb != added_emb, "The vector should change on the primary"

        self.redis.execute_command('WAIT', 1, 5000)
        replica_emb = self.replica.execute_command('VEMB', self.test_key, item)
        assert replica_emb == updated_emb, \
            f"Vector on the replica after the update: {replica_emb}, on the primary: {updated_emb}"

        # Same element and vector, with an attribute: only the attribute changes.
        attr = '{"a":1}'
        self.redis.execute_command(
            'VADD', self.test_key, 'VALUES', 3, 0, 1, 0, item, 'SETATTR', attr)
        self.redis.execute_command('WAIT', 1, 5000)
        replica_attr = self.replica.execute_command('VGETATTR', self.test_key, item)
        assert replica_attr == attr.encode(), \
            f"Attribute on the replica after the update: {replica_attr}, expected {attr}"


class VAddUpdateAOF(TestCase):
    def getname(self):
        return "[regression] VADD update of an existing element: AOF"

    def estimated_runtime(self):
        return 0.5

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
        original_appendonly = self.redis.config_get('appendonly')['appendonly']
        item = f'{self.test_key}:item'
        attr = '{"a":1}'
        try:
            self.redis.config_set('appendonly', 'yes')
            info = self._wait_rewrite_done()
            assert info['aof_last_bgrewrite_status'] == 'ok', \
                "Initial AOF rewrite should succeed"

            self.redis.execute_command(
                'VADD', self.test_key, 'VALUES', 3, 1, 0, 0, item)
            added_emb = self.redis.execute_command('VEMB', self.test_key, item)
            self.redis.execute_command(
                'VADD', self.test_key, 'VALUES', 3, 0, 1, 0, item)
            updated_emb = self.redis.execute_command('VEMB', self.test_key, item)
            assert updated_emb != added_emb, "The vector should change"

            self.redis.execute_command('DEBUG', 'LOADAOF')
            loaded_emb = self.redis.execute_command('VEMB', self.test_key, item)
            assert loaded_emb == updated_emb, \
                f"Vector after loading the AOF: {loaded_emb}, before: {updated_emb}"

            self.redis.execute_command(
                'VADD', self.test_key, 'VALUES', 3, 0, 1, 0, item, 'SETATTR', attr)
            self.redis.execute_command('DEBUG', 'LOADAOF')
            loaded_attr = self.redis.execute_command('VGETATTR', self.test_key, item)
            assert loaded_attr == attr.encode(), \
                f"Attribute after loading the AOF: {loaded_attr}, expected {attr}"
        finally:
            self.redis.config_set('appendonly', original_appendonly)
