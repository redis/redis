from test import TestCase, generate_random_vector, fill_redis_with_vectors
import struct

class VRANDMEMBERTest(TestCase):
    def getname(self):
        return "VRANDMEMBER basic functionality"

    def test(self):
        # Test with empty key
        result = self.redis.execute_command('VRANDMEMBER', self.test_key)
        assert result is None, "VRANDMEMBER on non-existent key should return NULL"

        result = self.redis.execute_command('VRANDMEMBER', self.test_key, 5)
        assert isinstance(result, list) and len(result) == 0, "VRANDMEMBER with count on non-existent key should return empty array"

        # Fill with vectors
        dim = 4
        count = 100
        data = fill_redis_with_vectors(self.redis, self.test_key, count, dim)

        # Test single random member
        result = self.redis.execute_command('VRANDMEMBER', self.test_key)
        assert result is not None, "VRANDMEMBER should return a random member"
        assert result.decode() in data.names, "Random member should be in the set"

        # Test multiple unique members (positive count)
        positive_count = 10
        result = self.redis.execute_command('VRANDMEMBER', self.test_key, positive_count)
        assert isinstance(result, list), "VRANDMEMBER with positive count should return an array"
        assert len(result) == positive_count, f"Should return {positive_count} members"

        # Check for uniqueness
        decoded_results = [r.decode() for r in result]
        assert len(decoded_results) == len(set(decoded_results)), "Results should be unique with positive count"
        for item in decoded_results:
            assert item in data.names, "All returned items should be in the set"

        # Test more members than in the set
        result = self.redis.execute_command('VRANDMEMBER', self.test_key, count + 10)
        assert len(result) == count, "Should return only the available members when asking for more than exist"

        # Test with duplicates (negative count)
        negative_count = -20
        result = self.redis.execute_command('VRANDMEMBER', self.test_key, negative_count)
        assert isinstance(result, list), "VRANDMEMBER with negative count should return an array"
        assert len(result) == abs(negative_count), f"Should return {abs(negative_count)} members"

        # Check that all returned elements are valid
        decoded_results = [r.decode() for r in result]
        for item in decoded_results:
            assert item in data.names, "All returned items should be in the set"

        # Test with count = 0 (edge case)
        result = self.redis.execute_command('VRANDMEMBER', self.test_key, 0)
        assert isinstance(result, list) and len(result) == 0, "VRANDMEMBER with count=0 should return empty array"
