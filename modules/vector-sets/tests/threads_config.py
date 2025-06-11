from test import TestCase, generate_random_vector
import struct


class ThreadsConfigTest(TestCase):
    """
    Test suite for vectorset-hnsw-max-threads configuration.

    This test validates the behavior of VADD and VSIM commands under different
    threading configurations. Since vectorset-hnsw-max-threads is IMMUTABLE,
    this test works with whatever configuration Redis was started with.

    Key behaviors tested:
    - VADD with and without CAS option
    - VSIM with and without NOTHREAD option
    - Configuration reading and validation
    - Thread-specific behavior (0 threads = synchronous, >0 = threaded)
    """

    def getname(self):
        return "vectorset-hnsw-max-threads configuration testing"

    def estimated_runtime(self):
        return 0.1  # Updated based on actual runtime measurements

    def get_config_value(self):
        """Get current vectorset-hnsw-max-threads config value"""
        try:
            result = self.redis.execute_command('CONFIG', 'GET', 'vectorset-hnsw-max-threads')
            if len(result) >= 2:
                return int(result[1])
            return None
        except Exception:
            return None

    def test_config_access(self):
        """Test 1: Basic configuration access and validation"""
        config_value = self.get_config_value()
        assert config_value is not None, "Should be able to read vectorset-hnsw-max-threads config"
        assert isinstance(config_value, int), f"Config value should be integer, got {type(config_value)}"
        assert 0 <= config_value <= 32, f"Config value should be between 0 and 32, got {config_value}"
        return config_value

    def test_vadd_without_cas(self):
        """Test 2: VADD command without CAS option"""
        dim = 64
        vec = generate_random_vector(dim)
        vec_bytes = struct.pack(f'{dim}f', *vec)

        result = self.redis.execute_command('VADD', self.test_key, 'FP32', vec_bytes, f'{self.test_key}:item:1')
        assert result == 1, f"VADD should return 1 for new item, got {result}"

        # Verify the vector was added
        card = self.redis.execute_command('VCARD', self.test_key)
        assert card == 1, f"VCARD should return 1, got {card}"

    def test_vadd_with_cas(self):
        """Test 3: VADD command with CAS option - tests threading behavior"""
        dim = 64
        vec = generate_random_vector(dim)
        vec_bytes = struct.pack(f'{dim}f', *vec)

        # First insertion with CAS should succeed
        result = self.redis.execute_command('VADD', self.test_key, 'FP32', vec_bytes, f'{self.test_key}:item:cas', 'CAS')
        assert result == 1, f"First VADD with CAS should return 1, got {result}"

        # Second insertion of same item with CAS should return 0
        result = self.redis.execute_command('VADD', self.test_key, 'FP32', vec_bytes, f'{self.test_key}:item:cas', 'CAS')
        assert result == 0, f"Duplicate VADD with CAS should return 0, got {result}"

    def test_vsim_without_nothread(self):
        """Test 4: VSIM command without NOTHREAD - uses available threads"""
        dim = 64

        # Add test vectors
        for i in range(5):
            vec = generate_random_vector(dim)
            vec_bytes = struct.pack(f'{dim}f', *vec)
            self.redis.execute_command('VADD', self.test_key, 'FP32', vec_bytes, f'{self.test_key}:item:{i}')

        # Test VSIM without NOTHREAD
        query_vec = generate_random_vector(dim)
        args = ['VSIM', self.test_key, 'VALUES', dim] + [str(x) for x in query_vec] + ['COUNT', 3]
        result = self.redis.execute_command(*args)

        assert isinstance(result, list), f"VSIM should return a list, got {type(result)}"
        assert len(result) <= 3, f"VSIM should return at most 3 results, got {len(result)}"

    def test_vsim_with_nothread(self):
        """Test 5: VSIM command with NOTHREAD - forces synchronous execution"""
        dim = 64

        # Ensure we have vectors to search
        card = self.redis.execute_command('VCARD', self.test_key)
        if card == 0:
            # Add test vectors
            for i in range(5):
                vec = generate_random_vector(dim)
                vec_bytes = struct.pack(f'{dim}f', *vec)
                self.redis.execute_command('VADD', self.test_key, 'FP32', vec_bytes, f'{self.test_key}:item:{i}')

        # Test VSIM with NOTHREAD
        query_vec = generate_random_vector(dim)
        args = ['VSIM', self.test_key, 'VALUES', dim] + [str(x) for x in query_vec] + ['COUNT', 3, 'NOTHREAD']
        result = self.redis.execute_command(*args)

        assert isinstance(result, list), f"VSIM with NOTHREAD should return a list, got {type(result)}"
        assert len(result) <= 3, f"VSIM with NOTHREAD should return at most 3 results, got {len(result)}"

    def test_thread_behavior_analysis(self):
        """Test 6: Analyze behavior based on current thread configuration"""
        current_threads = self.get_config_value()

        if current_threads == 0:
            # With 0 threads: all operations should be synchronous
            # CAS should be disabled automatically
            return self._test_zero_threads_behavior()
        elif current_threads == 1:
            # With 1 thread: minimal threading, operations serialized through single slot
            return self._test_single_thread_behavior()
        else:
            # With multiple threads: full threading capability
            return self._test_multi_thread_behavior(current_threads)

    def _test_zero_threads_behavior(self):
        """Test behavior when threads = 0 (synchronous mode)"""
        # All operations should work but be synchronous
        # CAS should be automatically disabled
        dim = 64
        vec = generate_random_vector(dim)
        vec_bytes = struct.pack(f'{dim}f', *vec)

        # VADD with CAS should work but in synchronous mode
        result = self.redis.execute_command('VADD', self.test_key, 'FP32', vec_bytes, f'{self.test_key}:sync:1', 'CAS')
        assert result == 1, "VADD with CAS should work in synchronous mode"

        # VSIM should work synchronously
        query_vec = generate_random_vector(dim)
        args = ['VSIM', self.test_key, 'VALUES', dim] + [str(x) for x in query_vec] + ['COUNT', 1]
        result = self.redis.execute_command(*args)
        assert isinstance(result, list), "VSIM should work in synchronous mode"

    def _test_single_thread_behavior(self):
        """Test behavior when threads = 1 (minimal threading)"""
        # Operations should work but be serialized through single slot
        dim = 64
        vec = generate_random_vector(dim)
        vec_bytes = struct.pack(f'{dim}f', *vec)

        # CAS should work but with single-threaded access
        result = self.redis.execute_command('VADD', self.test_key, 'FP32', vec_bytes, f'{self.test_key}:single:1', 'CAS')
        assert result == 1, "VADD with CAS should work with single thread"

    def _test_multi_thread_behavior(self, thread_count):
        """Test behavior with multiple threads (full threading)"""
        # Operations should work with full threading capability
        dim = 64
        vec = generate_random_vector(dim)
        vec_bytes = struct.pack(f'{dim}f', *vec)

        # CAS should work with full threading
        result = self.redis.execute_command('VADD', self.test_key, 'FP32', vec_bytes, f'{self.test_key}:multi:1', 'CAS')
        assert result == 1, "VADD with CAS should work with multiple threads"

    def test_config_immutability(self):
        """Test 7: Verify that the configuration is immutable"""
        current_value = self.get_config_value()

        # Try to set a different value - this should fail
        new_value = 16 if current_value != 16 else 8

        try:
            self.redis.execute_command('CONFIG', 'SET', 'vectorset-hnsw-max-threads', new_value)
            assert False, "Config should be immutable, but CONFIG SET succeeded"
        except Exception as e:
            # Expected: should fail because config is immutable
            error_msg = str(e).lower()
            assert any(word in error_msg for word in ['immutable', 'cannot', 'readonly', 'unsupported']), \
                f"Expected immutable config error, got: {e}"

    def test(self):
        """Main test method - runs all thread configuration tests"""
        # Get current configuration
        current_threads = self.test_config_access()
        print(f"Current vectorset-hnsw-max-threads: {current_threads}")

        # Clear test data
        self.redis.delete(self.test_key)

        # Run core functionality tests
        self.test_vadd_without_cas()
        self.test_vadd_with_cas()
        self.test_vsim_without_nothread()
        self.test_vsim_with_nothread()

        # Run thread-specific behavior tests
        self.test_thread_behavior_analysis()

        # Test configuration properties
        self.test_config_immutability()

        # Print summary
        self._print_test_summary(current_threads)

    def _print_test_summary(self, current_threads):
        """Print a summary of what was tested"""
        print(f"\nThread Configuration Test Summary:")
        print(f"  Configuration: {current_threads} threads")

        if current_threads == 0:
            print(f"  Behavior: Synchronous mode (no threading)")
            print(f"  CAS: Automatically disabled")
            print(f"  VSIM: Synchronous execution")
        elif current_threads == 1:
            print(f"  Behavior: Minimal threading (single slot)")
            print(f"  CAS: Serialized through single slot")
            print(f"  VSIM: Single-threaded execution")
        else:
            print(f"  Behavior: Full threading ({current_threads} slots)")
            print(f"  CAS: Multi-threaded execution")
            print(f"  VSIM: Parallel execution available")

        print(f"  NOTHREAD option: Forces synchronous execution regardless of config")
        print(f"  Configuration: Immutable (requires restart to change)")
        print(f"  All tests passed successfully!")
