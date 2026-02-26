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

        # Test extreme values with 512 dimensions to stress-test overflow safety
        vec4 = [1.0] * 512  # All +127 after quantization
        vec5 = [-1.0] * 512  # All -127 after quantization
        vec6 = [1.0, -1.0] * 256  # Alternating +127, -127

        # Add vectors using VALUES format with Q8 quantization
        self.redis.execute_command('VADD', f'{self.test_key}:extreme', 'VALUES', 512,
                                 *[str(x) for x in vec4], f'{self.test_key}:extreme:vec4', 'Q8')
        self.redis.execute_command('VADD', f'{self.test_key}:extreme', 'VALUES', 512,
                                 *[str(x) for x in vec5], f'{self.test_key}:extreme:vec5', 'Q8')
        self.redis.execute_command('VADD', f'{self.test_key}:extreme', 'VALUES', 512,
                                 *[str(x) for x in vec6], f'{self.test_key}:extreme:vec6', 'Q8')

        # Query vec4 against itself - worst-case positive accumulation (512 * 127 * 127 = 8,258,048)
        result_vec4 = self.redis.execute_command('VSIM', f'{self.test_key}:extreme', 'VALUES', 512,
                                               *[str(x) for x in vec4], 'WITHSCORES')
        results_vec4 = {}
        for i in range(0, len(result_vec4), 2):
            key = result_vec4[i].decode()
            score = float(result_vec4[i+1])
            results_vec4[key] = score

        # Verify extreme value handling
        # VSIM returns similarity = 1.0 - distance/2.0, so:
        # - Distance 0 (identical) → similarity 1.0
        # - Distance 2 (opposite) → similarity 0.0
        assert results_vec4[f'{self.test_key}:extreme:vec4'] > 0.999, \
            f"vec4 self-similarity should be very high, got {results_vec4[f'{self.test_key}:extreme:vec4']}"
        assert results_vec4[f'{self.test_key}:extreme:vec5'] < 0.01, \
            f"vec4 vs vec5 (opposite extremes) should be near 0, got {results_vec4[f'{self.test_key}:extreme:vec5']}"
        
        # Alternating pattern should result in mid-range similarity (perpendicular)
        assert 0.4 < results_vec4[f'{self.test_key}:extreme:vec6'] < 0.6, \
            f"vec4 vs vec6 (alternating) should be near 0.5, got {results_vec4[f'{self.test_key}:extreme:vec6']}"
