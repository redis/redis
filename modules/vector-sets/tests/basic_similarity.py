from test import TestCase

class BasicSimilarity(TestCase):
    def getname(self):
        return "VSIM reported distance makes sense with 4D vectors"

    def test(self):
        # Add two very similar vectors, one different
        vec1 = [1, 0, 0, 0]
        vec2 = [0.99, 0.01, 0, 0]
        vec3 = [0.1, 1, -1, 0.5]

        # Add vectors using VALUES format
        self.redis.execute_command('VADD', self.test_key, 'VALUES', 4, 
                                 *[str(x) for x in vec1], f'{self.test_key}:item:1')
        self.redis.execute_command('VADD', self.test_key, 'VALUES', 4, 
                                 *[str(x) for x in vec2], f'{self.test_key}:item:2')
        self.redis.execute_command('VADD', self.test_key, 'VALUES', 4, 
                                 *[str(x) for x in vec3], f'{self.test_key}:item:3')

        # Query similarity with vec1
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4, 
                                          *[str(x) for x in vec1], 'WITHSCORES')

        # Convert results to dictionary
        results_dict = {}
        for i in range(0, len(result), 2):
            key = result[i].decode()
            score = float(result[i+1])
            results_dict[key] = score

        # Verify results
        assert results_dict[f'{self.test_key}:item:1'] > 0.99, "Self-similarity should be very high"
        assert results_dict[f'{self.test_key}:item:2'] > 0.99, "Similar vector should have high similarity"
        assert results_dict[f'{self.test_key}:item:3'] < 0.8, "Not very similar vector should have low similarity"
