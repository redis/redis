from test import TestCase, fill_redis_with_vectors, generate_random_vector
import random

class LargeScale(TestCase):
    def getname(self):
        return "Large Scale Comparison"

    def estimated_runtime(self):
        return 10

    def test(self):
        dim = 300
        count = 20000
        k = 50

        # Fill Redis and get reference data for comparison
        random.seed(42)  # Make test deterministic
        data = fill_redis_with_vectors(self.redis, self.test_key, count, dim)

        # Generate query vector
        query_vec = generate_random_vector(dim)

        # Get results from Redis with good exploration factor
        redis_raw = self.redis.execute_command('VSIM', self.test_key, 'VALUES', dim, 
                                             *[str(x) for x in query_vec],
                                             'COUNT', k, 'WITHSCORES', 'EF', 500)

        # Convert Redis results to dict
        redis_results = {}
        for i in range(0, len(redis_raw), 2):
            key = redis_raw[i].decode()
            score = float(redis_raw[i+1])
            redis_results[key] = score

        # Get results from linear scan
        linear_results = data.find_k_nearest(query_vec, k)
        linear_items = {name: score for name, score in linear_results}

        # Compare overlap
        redis_set = set(redis_results.keys())
        linear_set = set(linear_items.keys())
        overlap = len(redis_set & linear_set)

        # If test fails, print comparison for debugging
        if overlap < k * 0.7:
            data.print_comparison({'items': redis_results, 'query_vector': query_vec}, k)

        assert overlap >= k * 0.7, \
            f"Expected at least 70% overlap in top {k} results, got {overlap/k*100:.1f}%"

        # Verify scores for common items
        for item in redis_set & linear_set:
            redis_score = redis_results[item]
            linear_score = linear_items[item]
            assert abs(redis_score - linear_score) < 0.01, \
                f"Score mismatch for {item}: Redis={redis_score:.3f} Linear={linear_score:.3f}"
