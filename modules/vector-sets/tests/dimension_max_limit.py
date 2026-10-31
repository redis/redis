from test import TestCase, generate_random_vector
import struct
import redis.exceptions

MAX_DIM = 65536


class DimensionMaxLimitVaddAtLimit(TestCase):
    def getname(self):
        return "[regression] VADD VALUES dim == MAX_DIM accepted"

    def estimated_runtime(self):
        return 0.5

    def test(self):
        dim = MAX_DIM
        vec = generate_random_vector(dim)

        result = self.redis.execute_command(
            'VADD', self.test_key,
            'VALUES', dim,
            *[str(x) for x in vec],
            f"{self.test_key}:item:maxdim")
        assert result == 1, "VADD with dimension at the limit should succeed"


class DimensionMaxLimitVaddAboveLimit(TestCase):
    def getname(self):
        return "[regression] VADD VALUES dim > MAX_DIM rejected"

    def estimated_runtime(self):
        return 0.1

    def test(self):
        too_big_dim = MAX_DIM + 1
        too_big_vec = generate_random_vector(16)
        try:
            self.redis.execute_command(
                'VADD', self.test_key,
                'VALUES', too_big_dim,
                *[str(x) for x in too_big_vec],
                f"{self.test_key}:item:toolarge")
            assert False, "VADD with dimension above the limit should fail"
        except redis.exceptions.ResponseError as e:
            # parseVector returns NULL so caller uses the generic invalid spec error
            assert "invalid vector specification" in str(e), (
                f"Expected invalid vector specification error, got: {e}")


class DimensionMaxLimitVsimAtLimit(TestCase):
    def getname(self):
        return "[regression] VSIM VALUES dim == MAX_DIM accepted"

    def estimated_runtime(self):
        return 0.5

    def test(self):
        # Insert a vector at the maximum allowed dimension, then query at the same dimension.
        dim = MAX_DIM
        base_vec = generate_random_vector(dim)

        result = self.redis.execute_command(
            'VADD', self.test_key,
            'VALUES', dim,
            *[str(x) for x in base_vec],
            f"{self.test_key}:item:1")
        assert result == 1, "VADD with dimension at the limit should succeed"

        query = generate_random_vector(dim)
        res = self.redis.execute_command(
            'VSIM', self.test_key,
            'VALUES', dim,
            *[str(x) for x in query],
            'COUNT', 1)
        assert isinstance(res, list), "VSIM with dimension at the limit should return a list"


class DimensionMaxLimitVsimAboveLimit(TestCase):
    def getname(self):
        return "[regression] VSIM VALUES dim > MAX_DIM rejected"

    def estimated_runtime(self):
        return 0.1

    def test(self):
        # Create a small index, then issue a VSIM with an over-limit dimension.
        base_dim = 16
        base_vec = generate_random_vector(base_dim)
        result = self.redis.execute_command(
            'VADD', self.test_key,
            'VALUES', base_dim,
            *[str(x) for x in base_vec],
            f"{self.test_key}:item:1")
        assert result == 1, "VADD with base_dim should succeed"

        too_big_dim = MAX_DIM + 1
        too_big_vec = generate_random_vector(16)
        try:
            self.redis.execute_command(
                'VSIM', self.test_key,
                'VALUES', too_big_dim,
                *[str(x) for x in too_big_vec],
                'COUNT', 1)
            assert False, "VSIM with dimension above the limit should fail"
        except redis.exceptions.ResponseError as e:
            assert "invalid vector specification" in str(e), (
                f"Expected invalid vector specification error in VSIM, got: {e}")


class DimensionMaxLimitHugeDimension(TestCase):
    def getname(self):
        return "[regression] VADD VALUES absurdly large dim rejected"

    def estimated_runtime(self):
        return 0.1

    def test(self):
        # Extremely large dimension close to LLONG_MAX should also be rejected safely.
        huge_dim = 9223372036854775807  # LLONG_MAX from the original report
        try:
            self.redis.execute_command(
                'VADD', self.test_key,
                'VALUES', huge_dim,
                '0')  # Just a dummy value; parseVector should reject based on dimension alone
            assert False, "VADD with absurdly large dimension should fail"
        except redis.exceptions.ResponseError as e:
            assert "invalid vector specification" in str(e), (
                f"Expected invalid vector specification error for huge dim, got: {e}")

