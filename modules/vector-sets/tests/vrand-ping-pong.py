from test import TestCase, generate_random_vector
import struct

class VRANDMEMBERPingPongRegressionTest(TestCase):
    def getname(self):
        return "[regression] VRANDMEMBER ping-pong"

    def test(self):
        """
        This test ensures that when only two vectors exist, VRANDMEMBER
        does not get stuck returning only one of them due to the "ping-pong" issue.
        """
        self.redis.delete(self.test_key) # Clean up before test
        dim = 4

        # Add exactly two vectors
        vec1_name = "vec1"
        vec1_data = generate_random_vector(dim)
        self.redis.execute_command('VADD', self.test_key, 'VALUES', dim, *vec1_data, vec1_name)

        vec2_name = "vec2"
        vec2_data = generate_random_vector(dim)
        self.redis.execute_command('VADD', self.test_key, 'VALUES', dim, *vec2_data, vec2_name)

        # Call VRANDMEMBER many times and check for distribution
        iterations = 100
        results = []
        for _ in range(iterations):
            member = self.redis.execute_command('VRANDMEMBER', self.test_key)
            results.append(member.decode())

        # Verify that both members were returned, proving it's not stuck
        unique_results = set(results)

        assert len(unique_results) == 2, f"Ping-pong test failed: should have returned 2 unique members, but got {len(unique_results)}."
