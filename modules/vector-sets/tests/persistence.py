from test import TestCase, fill_redis_with_vectors, generate_random_vector
import random

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
