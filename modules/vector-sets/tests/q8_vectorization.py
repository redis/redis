from test import TestCase

class Q8Vectorization(TestCase):
    def getname(self):
        return "Q8 quantization: verify vectorized vs scalar paths produce consistent results"

    def test(self):
        # Test with different dimensions to exercise different code paths:
        # - dim=16: Scalar path (< 32)
        # - dim=64: AVX2 path if available (>= 32, < 64 for AVX512)
        # - dim=128: AVX512 path if available (>= 64)
        
        test_dims = [16, 64, 128]
        
        for dim in test_dims:
            # Add two very similar vectors, one different
            vec1 = [1.0] * dim
            vec2 = [0.99] * dim 
            vec3 = [-1.0] * dim  # Opposite direction - should have low similarity
            
            # Add vectors with Q8 quantization
            self.redis.execute_command('VADD', f'{self.test_key}:dim{dim}', 'VALUES', dim, 
                                     *[str(x) for x in vec1], f'{self.test_key}:dim{dim}:item:1', 'Q8')
            self.redis.execute_command('VADD', f'{self.test_key}:dim{dim}', 'VALUES', dim, 
                                     *[str(x) for x in vec2], f'{self.test_key}:dim{dim}:item:2', 'Q8')
            self.redis.execute_command('VADD', f'{self.test_key}:dim{dim}', 'VALUES', dim, 
                                     *[str(x) for x in vec3], f'{self.test_key}:dim{dim}:item:3', 'Q8')
            
            # Query similarity
            result = self.redis.execute_command('VSIM', f'{self.test_key}:dim{dim}', 'VALUES', dim, 
                                              *[str(x) for x in vec1], 'WITHSCORES')
            
            # Convert results to dictionary
            results_dict = {}
            for i in range(0, len(result), 2):
                key = result[i].decode()
                score = float(result[i+1])
                results_dict[key] = score
            
            # Verify results are consistent across dimensions
            # Self-similarity should be very high
            assert results_dict[f'{self.test_key}:dim{dim}:item:1'] > 0.99, \
                f"Dim {dim}: Self-similarity too low: {results_dict[f'{self.test_key}:dim{dim}:item:1']}"
            
            # Similar vector should have high similarity
            assert results_dict[f'{self.test_key}:dim{dim}:item:2'] > 0.99, \
                f"Dim {dim}: Similar vector similarity too low: {results_dict[f'{self.test_key}:dim{dim}:item:2']}"
            
            # Opposite vector should have very low similarity (close to 0 or negative)
            assert results_dict[f'{self.test_key}:dim{dim}:item:3'] < 0.1, \
                f"Dim {dim}: Opposite vector similarity too high: {results_dict[f'{self.test_key}:dim{dim}:item:3']}"
