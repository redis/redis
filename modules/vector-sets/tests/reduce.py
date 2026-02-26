from test import TestCase, fill_redis_with_vectors, generate_random_vector

class Reduce(TestCase):
    def getname(self):
        return "Dimension Reduction"

    def estimated_runtime(self):
        return 0.2

    def test(self):
        original_dim = 100
        reduced_dim = 80
        count = 1000
        k = 50  # Number of nearest neighbors to check

        # Fill Redis with vectors using REDUCE and get reference data
        data = fill_redis_with_vectors(self.redis, self.test_key, count, original_dim, reduced_dim)

        # Verify dimension is reduced
        dim = self.redis.execute_command('VDIM', self.test_key)
        assert dim == reduced_dim, f"Expected dimension {reduced_dim}, got {dim}"

        # Generate query vector and get nearest neighbors using Redis
        query_vec = generate_random_vector(original_dim)
        redis_raw = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 
                                             original_dim, *[str(x) for x in query_vec],
                                             'COUNT', k, 'WITHSCORES')

        # Convert Redis results to dict
        redis_results = {}
        for i in range(0, len(redis_raw), 2):
            key = redis_raw[i].decode()
            score = float(redis_raw[i+1])
            redis_results[key] = score

        # Get results from linear scan with original vectors
        linear_results = data.find_k_nearest(query_vec, k)
        linear_items = {name: score for name, score in linear_results}

        # Compare overlap between reduced and non-reduced results
        redis_set = set(redis_results.keys())
        linear_set = set(linear_items.keys())
        overlap = len(redis_set & linear_set)
        overlap_ratio = overlap / k

        # With random projection, we expect some loss of accuracy but should
        # maintain at least some similarity structure.
        # Note that gaussian distribution is the worse with this test, so
        # in real world practice, things will be better.
        min_expected_overlap = 0.1  # At least 10% overlap in top-k
        assert overlap_ratio >= min_expected_overlap, \
            f"Dimension reduction lost too much structure. Only {overlap_ratio*100:.1f}% overlap in top {k}"

        # For items that appear in both results, scores should be reasonably correlated
        common_items = redis_set & linear_set
        for item in common_items:
            redis_score = redis_results[item]
            linear_score = linear_items[item]
            # Allow for some deviation due to dimensionality reduction
            assert abs(redis_score - linear_score) < 0.2, \
                f"Score mismatch too high for {item}: Redis={redis_score:.3f} Linear={linear_score:.3f}"

        # If test fails, print comparison for debugging
        if overlap_ratio < min_expected_overlap:
            print("\nLow overlap in results. Details:")
            print("\nTop results from linear scan (original vectors):")
            for name, score in linear_results:
                print(f"{name}: {score:.3f}")
            print("\nTop results from Redis (reduced vectors):")
            for item, score in sorted(redis_results.items(), key=lambda x: x[1], reverse=True):
                print(f"{item}: {score:.3f}")
