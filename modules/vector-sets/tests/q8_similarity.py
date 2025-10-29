from test import TestCase

class Q8Similarity(TestCase):
    def getname(self):
        return "Q8 quantization: VSIM reported distance makes sense with 4D vectors"

    def test(self):
        # Add two very similar vectors, one different
        # Using same test vectors as basic_similarity.py for comparison
        vec1 = [1, 0, 0, 0]
        vec2 = [0.99, 0.01, 0, 0]
        vec3 = [0.1, 1, -1, 0.5]

        # Add vectors using VALUES format with Q8 quantization
        self.redis.execute_command('VADD', self.test_key, 'VALUES', 4, 
                                 *[str(x) for x in vec1], f'{self.test_key}:item:1', 'Q8')
        self.redis.execute_command('VADD', self.test_key, 'VALUES', 4, 
                                 *[str(x) for x in vec2], f'{self.test_key}:item:2', 'Q8')
        self.redis.execute_command('VADD', self.test_key, 'VALUES', 4, 
                                 *[str(x) for x in vec3], f'{self.test_key}:item:3', 'Q8')

        # Query similarity with vec1
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4, 
                                          *[str(x) for x in vec1], 'WITHSCORES')

        # Convert results to dictionary
        results_dict = {}
        for i in range(0, len(result), 2):
            key = result[i].decode()
            score = float(result[i+1])
            results_dict[key] = score

        # Verify results (same expectations as float32, allowing for quantization error)
        assert results_dict[f'{self.test_key}:item:1'] > 0.99, "Self-similarity should be very high (Q8)"
        assert results_dict[f'{self.test_key}:item:2'] > 0.99, "Similar vector should have high similarity (Q8)"
        assert results_dict[f'{self.test_key}:item:3'] < 0.80, "Not very similar vector should have low similarity (Q8)"
