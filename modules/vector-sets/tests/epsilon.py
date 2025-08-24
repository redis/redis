from test import TestCase

class EpsilonOption(TestCase):
    def getname(self):
        return "VSIM EPSILON option filtering"

    def estimated_runtime(self):
        return 0.1

    def test(self):
        # Add vectors as shown in the example
        # Vector 'a' at (1, 1) - normalized to (0.707, 0.707)
        result = self.redis.execute_command('VADD', self.test_key, 'VALUES', '2', '1', '1', 'a')
        assert result == 1, "VADD should return 1 for item 'a'"

        # Vector 'b' at (0, 1) - normalized to (0, 1)
        result = self.redis.execute_command('VADD', self.test_key, 'VALUES', '2', '0', '1', 'b')
        assert result == 1, "VADD should return 1 for item 'b'"

        # Vector 'c' at (0, 0) - this will be a zero vector, might be handled specially
        result = self.redis.execute_command('VADD', self.test_key, 'VALUES', '2', '0', '0', 'c')
        assert result == 1, "VADD should return 1 for item 'c'"

        # Vector 'd' at (0, -1) - normalized to (0, -1)
        result = self.redis.execute_command('VADD', self.test_key, 'VALUES', '2', '0', '-1', 'd')
        assert result == 1, "VADD should return 1 for item 'd'"

        # Vector 'e' at (-1, -1) - normalized to (-0.707, -0.707)
        result = self.redis.execute_command('VADD', self.test_key, 'VALUES', '2', '-1', '-1', 'e')
        assert result == 1, "VADD should return 1 for item 'e'"

        # Test without EPSILON - should return all items
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', '2', '1', '1', 'WITHSCORES')
        # Result is a flat list: [elem1, score1, elem2, score2, ...]
        elements_all = [result[i].decode() for i in range(0, len(result), 2)]
        scores_all = [float(result[i]) for i in range(1, len(result), 2)]

        assert len(elements_all) == 5, f"Should return 5 elements without EPSILON, got {len(elements_all)}"
        assert elements_all[0] == 'a', "First element should be 'a' (most similar)"
        assert scores_all[0] == 1.0, "Score for 'a' should be 1.0 (identical)"

        # Test with EPSILON 0.5 - should return only elements with similarity >= 0.5 (distance < 0.5)
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', '2', '1', '1', 'WITHSCORES', 'EPSILON', '0.5')
        elements_epsilon_0_5 = [result[i].decode() for i in range(0, len(result), 2)]
        scores_epsilon_0_5 = [float(result[i]) for i in range(1, len(result), 2)]

        assert len(elements_epsilon_0_5) == 3, f"With EPSILON 0.5, should return 3 elements, got {len(elements_epsilon_0_5)}"
        assert set(elements_epsilon_0_5) == {'a', 'b', 'c'}, f"With EPSILON 0.5, should get a, b, c, got {elements_epsilon_0_5}"

        # Verify all returned scores are >= 0.5
        for i, score in enumerate(scores_epsilon_0_5):
            assert score >= 0.5, f"Element {elements_epsilon_0_5[i]} has score {score} which is < 0.5"

        # Test with EPSILON 0.2 - should return only elements with similarity >= 0.8 (distance < 0.2)
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', '2', '1', '1', 'WITHSCORES', 'EPSILON', '0.2')
        elements_epsilon_0_2 = [result[i].decode() for i in range(0, len(result), 2)]
        scores_epsilon_0_2 = [float(result[i]) for i in range(1, len(result), 2)]

        assert len(elements_epsilon_0_2) == 2, f"With EPSILON 0.2, should return 2 elements, got {len(elements_epsilon_0_2)}"
        assert set(elements_epsilon_0_2) == {'a', 'b'}, f"With EPSILON 0.2, should get a, b, got {elements_epsilon_0_2}"

        # Verify all returned scores are >= 0.8 (since distance < 0.2 means similarity > 0.8)
        for i, score in enumerate(scores_epsilon_0_2):
            assert score >= 0.8, f"Element {elements_epsilon_0_2[i]} has score {score} which is < 0.8"

        # Test with very small EPSILON - should return only the exact match
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', '2', '1', '1', 'WITHSCORES', 'EPSILON', '0.001')
        elements_epsilon_small = [result[i].decode() for i in range(0, len(result), 2)]

        assert len(elements_epsilon_small) == 1, f"With EPSILON 0.001, should return only 1 element, got {len(elements_epsilon_small)}"
        assert elements_epsilon_small[0] == 'a', "With very small EPSILON, should only get 'a'"

        # Test with EPSILON 1.0 - should return all elements (since all similarities are between 0 and 1)
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', '2', '1', '1', 'WITHSCORES', 'EPSILON', '1.0')
        elements_epsilon_1 = [result[i].decode() for i in range(0, len(result), 2)]

        assert len(elements_epsilon_1) == 5, f"With EPSILON 1.0, should return all 5 elements, got {len(elements_epsilon_1)}"
