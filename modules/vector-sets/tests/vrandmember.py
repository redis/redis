from test import TestCase, generate_random_vector, fill_redis_with_vectors
import struct
import redis.exceptions

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


class VRANDMEMBERCountOutOfRangeTest(TestCase):
    def getname(self):
        return "VRANDMEMBER COUNT=LLONG_MIN is rejected, not crashing"

    def test(self):
        llong_min = -9223372036854775808

        # LLONG_MIN has no representable positive magnitude, so negating it is
        # undefined behavior and the previous code produced a negative reply
        # array length, crashing the server via serverAssert(length >= 0)
        # (issue #15384). The invalid count is now rejected up front, before the
        # missing-key / empty-set fast paths, so it is caught regardless of
        # whether the set has any members.

        # Missing key: previously returned an empty array without ever looking
        # at the count; it must be rejected too.
        try:
            self.redis.execute_command('VRANDMEMBER', self.test_key, llong_min)
            assert False, "VRANDMEMBER with LLONG_MIN count should error even for a missing key"
        except redis.exceptions.ResponseError as e:
            assert "out of range" in str(e).lower(), f"unexpected error: {e}"

        # Non-empty set: same rejection.
        fill_redis_with_vectors(self.redis, self.test_key, 10, 4)
        try:
            self.redis.execute_command('VRANDMEMBER', self.test_key, llong_min)
            assert False, "VRANDMEMBER with LLONG_MIN count should return an error"
        except redis.exceptions.ResponseError as e:
            assert "out of range" in str(e).lower(), f"unexpected error: {e}"

        # The server must still be responsive, i.e. it did not crash.
        assert self.redis.execute_command('PING'), "server should be alive after rejecting the count"
