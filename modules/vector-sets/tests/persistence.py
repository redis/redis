from test import TestCase, fill_redis_with_vectors, generate_random_vector
import random
import os
import shutil

class HNSWPersistence(TestCase):
    def getname(self):
        return "HNSW Persistence"

    def estimated_runtime(self):
        return 30

    def _verify_results(self, key, dim, query_vec, reduced_dim=None):
        """Run a query and return results dict"""
        k = 10
        args = ['VSIM', key]

        if reduced_dim:
            args.extend(['VALUES', dim])
            args.extend([str(x) for x in query_vec])
        else:
            args.extend(['VALUES', dim])
            args.extend([str(x) for x in query_vec])

        args.extend(['COUNT', k, 'WITHSCORES'])
        results = self.redis.execute_command(*args)

        results_dict = {}
        for i in range(0, len(results), 2):
            key = results[i].decode()
            score = float(results[i+1])
            results_dict[key] = score
        return results_dict

    def test(self):
        # Setup dimensions
        dim = 128
        reduced_dim = 32
        count = 5000
        random.seed(42)

        # Create two datasets - one normal and one with dimension reduction
        normal_data = fill_redis_with_vectors(self.redis, f"{self.test_key}:normal", count, dim)
        projected_data = fill_redis_with_vectors(self.redis, f"{self.test_key}:projected",
                                               count, dim, reduced_dim)

        # Generate query vectors we'll use before and after reload
        query_vec_normal = generate_random_vector(dim)
        query_vec_projected = generate_random_vector(dim)

        # Get initial results for both sets
        initial_normal = self._verify_results(f"{self.test_key}:normal", 
                                            dim, query_vec_normal)
        initial_projected = self._verify_results(f"{self.test_key}:projected", 
                                               dim, query_vec_projected, reduced_dim)

        # Force Redis to save and reload the dataset
        self.redis.execute_command('DEBUG', 'RELOAD')

        # Verify results after reload
        reloaded_normal = self._verify_results(f"{self.test_key}:normal", 
                                             dim, query_vec_normal)
        reloaded_projected = self._verify_results(f"{self.test_key}:projected", 
                                                dim, query_vec_projected, reduced_dim)

        # Verify normal vectors results
        assert len(initial_normal) == len(reloaded_normal), \
            "Normal vectors: Result count mismatch before/after reload"

        for key in initial_normal:
            assert key in reloaded_normal, f"Normal vectors: Missing item after reload: {key}"
            assert abs(initial_normal[key] - reloaded_normal[key]) < 0.0001, \
                f"Normal vectors: Score mismatch for {key}: " + \
                f"before={initial_normal[key]:.6f}, after={reloaded_normal[key]:.6f}"

        # Verify projected vectors results
        assert len(initial_projected) == len(reloaded_projected), \
            "Projected vectors: Result count mismatch before/after reload"

        for key in initial_projected:
            assert key in reloaded_projected, \
                f"Projected vectors: Missing item after reload: {key}"
            assert abs(initial_projected[key] - reloaded_projected[key]) < 0.0001, \
                f"Projected vectors: Score mismatch for {key}: " + \
                f"before={initial_projected[key]:.6f}, after={reloaded_projected[key]:.6f}"

        self.redis.delete(f"{self.test_key}:normal")
        self.redis.delete(f"{self.test_key}:projected")


class CorruptedRDBDimZero(TestCase):
    """Test that corrupted RDB with dim=0 doesn't cause division by zero."""

    def getname(self):
        return "Corrupted RDB dim=0 division by zero"

    def estimated_runtime(self):
        return 2

    def test(self):
        script_dir = os.path.dirname(os.path.abspath(__file__))
        corrupted_rdb = os.path.join(script_dir, 'assets', 'corrupted_dim_zero.rdb')

        if not os.path.exists(corrupted_rdb):
            raise Exception(f"Test asset not found: {corrupted_rdb}")

        # Get RDB path
        rdb_dir = self.redis.execute_command('CONFIG', 'GET', 'dir')[1]
        rdb_file = self.redis.execute_command('CONFIG', 'GET', 'dbfilename')[1]
        if isinstance(rdb_dir, bytes): rdb_dir = rdb_dir.decode()
        if isinstance(rdb_file, bytes): rdb_file = rdb_file.decode()
        rdb_path = os.path.join(rdb_dir, rdb_file)

        # Backup and load corrupted RDB
        backup = rdb_path + '.bak'
        if os.path.exists(rdb_path):
            shutil.copy(rdb_path, backup)

        try:
            shutil.copy(corrupted_rdb, rdb_path)
            self.redis.execute_command('DEBUG', 'RELOAD', 'NOSAVE')

            # If we get here, RDB loaded - check for NaN
            dim = self.redis.execute_command('VDIM', 'test:dimzero')
            if dim == 0:
                results = self.redis.execute_command('VSIM', 'test:dimzero',
                    'ELE', 'elem1', 'WITHSCORES')
                scores = [r.decode() if isinstance(r, bytes) else r
                          for r in results[1::2]]
                assert not any(s in ('nan', 'NaN') for s in scores), \
                    f"NaN scores returned: {results}"

        except Exception as e:
            # RDB rejected is acceptable (means fix is working)
            if 'load' not in str(e).lower() and 'dimension' not in str(e).lower():
                raise

        finally:
            if os.path.exists(backup):
                shutil.copy(backup, rdb_path)
                os.remove(backup)
                try:
                    self.redis.execute_command('DEBUG', 'RELOAD', 'NOSAVE')
                except:
                    pass
            self.redis.delete('test:dimzero')
