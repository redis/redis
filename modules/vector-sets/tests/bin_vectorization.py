from test import TestCase

class BinVectorization(TestCase):
    def getname(self):
        return "Binary quantization: verify vectorized vs scalar paths produce consistent results"

    def test(self):
        # Test with different dimensions to exercise different code paths:
        # - dim=1: Edge case for minimal valid dimension (scalar path)
        # - dim=64: Exact alignment boundary, one uint64_t word (scalar path)
        # - dim=128: Scalar path (< 256)
        # - dim=384: AVX2 path if available (>= 256, < 512)
        # - dim=768: AVX512 path if available (>= 512)
        # Note: dim=0 is not tested as it's invalid input (division by zero)
        
        test_dims = [1, 64, 128, 384, 768]
        
        for dim in test_dims:
            # Add two very similar vectors, one different
            vec1 = [1.0] * dim
            vec2 = [0.99] * dim  # Very similar to vec1
            vec3 = [-1.0] * dim  # Opposite direction - should have low similarity
            
            # Add vectors with binary quantization
            self.redis.execute_command('VADD', f'{self.test_key}:dim{dim}', 'VALUES', dim, 
                                     *[str(x) for x in vec1], f'{self.test_key}:dim{dim}:item:1', 'BIN')
            self.redis.execute_command('VADD', f'{self.test_key}:dim{dim}', 'VALUES', dim, 
                                     *[str(x) for x in vec2], f'{self.test_key}:dim{dim}:item:2', 'BIN')
            self.redis.execute_command('VADD', f'{self.test_key}:dim{dim}', 'VALUES', dim, 
                                     *[str(x) for x in vec3], f'{self.test_key}:dim{dim}:item:3', 'BIN')
            
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
            # Self-similarity should be very high (binary quantization is less precise)
            assert results_dict[f'{self.test_key}:dim{dim}:item:1'] > 0.99, \
                f"Dim {dim}: Self-similarity too low: {results_dict[f'{self.test_key}:dim{dim}:item:1']}"
            
            # Similar vector should have high similarity (binary quant loses some precision)
            assert results_dict[f'{self.test_key}:dim{dim}:item:2'] > 0.95, \
                f"Dim {dim}: Similar vector similarity too low: {results_dict[f'{self.test_key}:dim{dim}:item:2']}"
            
            # Opposite vector should have very low similarity
            assert results_dict[f'{self.test_key}:dim{dim}:item:3'] < 0.1, \
                f"Dim {dim}: Opposite vector similarity too high: {results_dict[f'{self.test_key}:dim{dim}:item:3']}"
