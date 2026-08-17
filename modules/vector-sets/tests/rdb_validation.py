from test import TestCase
import redis.exceptions


# MODULE_2 payload for vectorset encver 0 with dim=2 and elements=0.
EMPTY_VECTOR_SET_DUMP = bytes.fromhex(
    "0781bde72da2bb1eb400020202000250000200000e000000000000000000"
)


class EmptyVectorSetRdbLoad(TestCase):
    def getname(self):
        return "[regression] RESTORE rejects an empty vector set"

    def test(self):
        try:
            self.redis.execute_command(
                'RESTORE', self.test_key, 0, EMPTY_VECTOR_SET_DUMP, 'REPLACE')
            assert False, "RESTORE should reject an empty vector set"
        except redis.exceptions.ResponseError as error:
            assert "Bad data format" in str(error), (
                f"Expected a bad data format error, got: {error}")

        assert self.redis.exists(self.test_key) == 0, (
            "RESTORE must not install the invalid vector set")
        assert self.redis.ping(), "Redis should remain responsive after RESTORE"
