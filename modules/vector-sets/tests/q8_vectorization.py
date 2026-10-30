from test import TestCase

class Q8Vectorization(TestCase):
    def getname(self):
        return "Q8 quantization: verify vectorized vs scalar paths produce consistent results"

    def test(self):
        # Test with different dimensions to exercise different code paths and boundaries:
        # - dim=16: Scalar path (< 32)
        # - dim=31: Largest scalar-only dimension (boundary)
        # - dim=32: Smallest AVX2 dimension, no remainder (boundary)
        # - dim=33: AVX2 with 1-element remainder
        # - dim=63: AVX2 with 31-element remainder (largest AVX2-only)
        # - dim=64: Smallest AVX512 dimension, no remainder (boundary)
        # - dim=65: AVX512 with 1-element remainder
        # - dim=128: AVX512 path with no remainder
        # - dim=256, dim=512: Large dimensions to test overflow prevention
        
        test_dims = [16, 31, 32, 33, 63, 64, 65, 128, 256, 512]
        
        for dim in test_dims:
            key = f'{self.test_key}:dim{dim}'
            
            # Test vectors with extreme values to verify overflow prevention:
            # vec1: all +1.0 -> quantizes to +127 (max positive int8)
            # vec2: all +0.99 -> quantizes to ~+126 (similar to vec1)
            # vec3: all -1.0 -> quantizes to -127/-128 (max negative int8)
            # vec4: alternating +1.0/-1.0 -> alternating +127/-127 (tests mixed signs)
            vec1 = [1.0] * dim       # All max positive
            vec2 = [0.99] * dim      # Similar to vec1
            vec3 = [-1.0] * dim      # All max negative (opposite direction)
            vec4 = [1.0 if i % 2 == 0 else -1.0 for i in range(dim)]  # Alternating extreme values
            
            # Add vectors with Q8 quantization
            self.redis.execute_command('VADD', key, 'VALUES', dim, 
                                     *[str(x) for x in vec1], f'{key}:item:1', 'Q8')
            self.redis.execute_command('VADD', key, 'VALUES', dim, 
                                     *[str(x) for x in vec2], f'{key}:item:2', 'Q8')
            self.redis.execute_command('VADD', key, 'VALUES', dim, 
                                     *[str(x) for x in vec3], f'{key}:item:3', 'Q8')
            self.redis.execute_command('VADD', key, 'VALUES', dim, 
                                     *[str(x) for x in vec4], f'{key}:item:4', 'Q8')
            
            # Query similarity using vec1 (all max positive values)
            # This exercises worst-case positive accumulation: dim * 127 * 127
            result = self.redis.execute_command('VSIM', key, 'VALUES', dim, 
                                              *[str(x) for x in vec1], 'WITHSCORES')
            
            # Convert results to dictionary
            results_dict = {}
            for i in range(0, len(result), 2):
                k = result[i].decode()
                score = float(result[i+1])
                results_dict[k] = score
            
            # Verify results - these would be wrong if overflow occurred
            # Self-similarity should be ~1.0 (identical vectors)
            assert results_dict[f'{key}:item:1'] > 0.99, \
                f"Dim {dim}: Self-similarity too low: {results_dict[f'{key}:item:1']}"
            
            # Similar vector should have high similarity
            assert results_dict[f'{key}:item:2'] > 0.99, \
                f"Dim {dim}: Similar vector similarity too low: {results_dict[f'{key}:item:2']}"
            
            # Opposite vector should have very low similarity (~0.0)
            # With overflow bug, this could give incorrect positive values
            assert results_dict[f'{key}:item:3'] < 0.1, \
                f"Dim {dim}: Opposite vector similarity too high: {results_dict[f'{key}:item:3']}"
            
            # Alternating vector: dot product sums to ~0, so similarity ~0.5
            # (127*127) + (127*-127) + ... = 0, normalized gives ~0.5
            assert 0.4 < results_dict[f'{key}:item:4'] < 0.6, \
                f"Dim {dim}: Alternating vector similarity unexpected: {results_dict[f'{key}:item:4']}"
            
            # Also query with the alternating pattern to verify its self-similarity
            result_alt = self.redis.execute_command('VSIM', key, 'VALUES', dim,
                                                  *[str(x) for x in vec4], 'WITHSCORES')
            results_alt = {}
            for i in range(0, len(result_alt), 2):
                k = result_alt[i].decode()
                score = float(result_alt[i+1])
                results_alt[k] = score
            
            assert results_alt[f'{key}:item:4'] > 0.99, \
                f"Dim {dim}: Alternating self-similarity too low: {results_alt[f'{key}:item:4']}"
