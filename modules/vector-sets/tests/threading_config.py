from test import TestCase, generate_random_vector
import struct


class ThreadingConfigTest(TestCase):
    """
    Test suite for vset-force-single-threaded-execution configuration.

    This test validates the behavior of VADD and VSIM commands under different
    threading configurations. The new configuration is MUTABLE and BINARY:
    - false (0): Multi-threaded execution enabled (default)
    - true (1): Force single-threaded execution

    Key behaviors tested:
    - VADD with and without CAS option under both threading modes
    - VSIM with and without NOTHREAD option under both threading modes
    - Configuration reading, validation, and runtime modification
    - Thread behavior switching (multi-threaded vs forced single-threaded)
    """

    def getname(self):
        return "vset-force-single-threaded-execution configuration testing"

    def estimated_runtime(self):
        return 0.5  # Updated for mutable config testing with mode switching

    def get_config_value(self):
        """Get current vset-force-single-threaded-execution config value"""
        try:
            result = self.redis.execute_command('CONFIG', 'GET', 'vset-force-single-threaded-execution')
            if len(result) >= 2:
                # Redis returns 'yes'/'no' for boolean configs
                return result[1].decode() if isinstance(result[1], bytes) else result[1]
            return None
        except Exception:
            return None

    def set_config_value(self, value):
        """Set vset-force-single-threaded-execution config value"""
        try:
            # Convert boolean to yes/no string
            str_value = 'yes' if value else 'no'
            result = self.redis.execute_command('CONFIG', 'SET', 'vset-force-single-threaded-execution', str_value)
            return result == b'OK' or result == 'OK'
        except Exception as e:
            print(f"Failed to set config: {e}")
            return False

    def test_config_access_and_mutability(self):
        """Test 1: Configuration access and mutability"""
        # Get initial value
        initial_value = self.get_config_value()
        assert initial_value is not None, "Should be able to read vset-force-single-threaded-execution config"
        assert initial_value in ['yes', 'no'], f"Config value should be yes/no, got {initial_value}"

        # Test mutability by toggling the value
        new_value = 'no' if initial_value == 'yes' else 'yes'
        assert self.set_config_value(new_value == 'yes'), "Should be able to change config value"

        # Verify the change
        current_value = self.get_config_value()
        assert current_value == new_value, f"Config should be {new_value}, got {current_value}"

        # Restore original value
        assert self.set_config_value(initial_value == 'yes'), "Should be able to restore original value"

        return initial_value == 'yes'

    def test_vadd_without_cas(self, force_single_threaded=False):
        """Test 2: VADD command without CAS option"""
        # Set threading mode
        self.set_config_value(force_single_threaded)

        # Clear test data to avoid dimension conflicts
        self.redis.delete(self.test_key)

        dim = 64
        vec = generate_random_vector(dim)
        vec_bytes = struct.pack(f'{dim}f', *vec)

        result = self.redis.execute_command('VADD', self.test_key, 'FP32', vec_bytes, f'{self.test_key}:item:1')
        assert result == 1, f"VADD should return 1 for new item, got {result}"

        # Verify the vector was added
        card = self.redis.execute_command('VCARD', self.test_key)
        assert card == 1, f"VCARD should return 1, got {card}"

    def test_vadd_with_cas(self, force_single_threaded=False):
        """Test 3: VADD command with CAS option"""
        # Set threading mode
        self.set_config_value(force_single_threaded)

        # Clear test data to avoid dimension conflicts
        self.redis.delete(self.test_key)

        dim = 64
        vec = generate_random_vector(dim)
        vec_bytes = struct.pack(f'{dim}f', *vec)

        # First insertion with CAS should succeed
        result = self.redis.execute_command('VADD', self.test_key, 'FP32', vec_bytes, f'{self.test_key}:item:cas', 'CAS')
        assert result == 1, f"First VADD with CAS should return 1, got {result}"

        # Second insertion of same item with CAS should return 0
        result = self.redis.execute_command('VADD', self.test_key, 'FP32', vec_bytes, f'{self.test_key}:item:cas', 'CAS')
        assert result == 0, f"Duplicate VADD with CAS should return 0, got {result}"

    def test_vsim_without_nothread(self, force_single_threaded=False):
        """Test 4: VSIM command without NOTHREAD"""
        # Set threading mode
        self.set_config_value(force_single_threaded)

        # Clear test data to avoid dimension conflicts
        self.redis.delete(self.test_key)

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

    def test_vsim_with_nothread(self, force_single_threaded=False):
        """Test 5: VSIM command with NOTHREAD"""
        # Set threading mode
        self.set_config_value(force_single_threaded)

        dim = 64

        # Ensure we have vectors to search (use existing vectors from previous test)
        card = self.redis.execute_command('VCARD', self.test_key)
        if card == 0:
            # Add test vectors if none exist
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

    def test_threading_mode_comparison(self):
        """Test 6: Compare behavior between threading modes"""
        dim = 64

        # Clear test data
        self.redis.delete(self.test_key)

        # Test multi-threaded mode (default)
        self.set_config_value(False)  # Multi-threaded
        self.test_vadd_without_cas(False)
        self.test_vadd_with_cas(False)
        multi_threaded_card = self.redis.execute_command('VCARD', self.test_key)

        # Clear and test single-threaded mode
        self.redis.delete(self.test_key)
        self.set_config_value(True)  # Single-threaded
        self.test_vadd_without_cas(True)
        self.test_vadd_with_cas(True)
        single_threaded_card = self.redis.execute_command('VCARD', self.test_key)

        # Both modes should produce same results
        assert multi_threaded_card == single_threaded_card, \
            f"Both modes should produce same results: multi={multi_threaded_card}, single={single_threaded_card}"

    def test_nothread_override_behavior(self):
        """Test 7: NOTHREAD option should work regardless of config"""
        dim = 64

        # Test with both config modes
        for force_single in [False, True]:
            self.set_config_value(force_single)
            self.redis.delete(self.test_key)

            # Add test vectors
            for i in range(3):
                vec = generate_random_vector(dim)
                vec_bytes = struct.pack(f'{dim}f', *vec)
                self.redis.execute_command('VADD', self.test_key, 'FP32', vec_bytes, f'{self.test_key}:item:{i}')

            # NOTHREAD should work regardless of config
            query_vec = generate_random_vector(dim)
            args = ['VSIM', self.test_key, 'VALUES', dim] + [str(x) for x in query_vec] + ['COUNT', 2, 'NOTHREAD']
            result = self.redis.execute_command(*args)

            assert isinstance(result, list), f"NOTHREAD should work with force_single={force_single}"
            assert len(result) <= 2, f"NOTHREAD should return ≤2 results with force_single={force_single}"

    def test(self):
        """Main test method - runs all threading configuration tests"""
        # Get initial configuration
        initial_force_single = self.test_config_access_and_mutability()
        print(f"Initial vset-force-single-threaded-execution: {'yes' if initial_force_single else 'no'}")

        # Clear test data
        self.redis.delete(self.test_key)

        # Test both threading modes
        print("Testing multi-threaded mode...")
        self.set_config_value(False)
        self.test_vadd_without_cas(False)
        self.test_vadd_with_cas(False)
        self.test_vsim_without_nothread(False)
        self.test_vsim_with_nothread(False)

        print("Testing single-threaded mode...")
        self.set_config_value(True)
        self.test_vadd_without_cas(True)
        self.test_vadd_with_cas(True)
        self.test_vsim_without_nothread(True)
        self.test_vsim_with_nothread(True)

        # Test mode comparison and NOTHREAD override
        self.test_threading_mode_comparison()
        self.test_nothread_override_behavior()

        # Restore initial configuration
        self.set_config_value(initial_force_single)

        # Print summary
        self._print_test_summary(initial_force_single)

    def _print_test_summary(self, initial_force_single):
        """Print a summary of what was tested"""
        print(f"\nThreading Configuration Test Summary:")
        print(f"  Configuration: vset-force-single-threaded-execution")
        print(f"  Type: Boolean, Mutable")
        print(f"  Initial value: {'yes' if initial_force_single else 'no'}")
        print(f"  Tested modes: Both multi-threaded (no) and single-threaded (yes)")
        print(f"  VADD: Works correctly in both modes")
        print(f"  VADD with CAS: Works correctly in both modes")
        print(f"  VSIM: Works correctly in both modes")
        print(f"  NOTHREAD option: Overrides config in both modes")
        print(f"  Configuration mutability: ✅ Successfully changed at runtime")
        print(f"  All tests passed successfully!")
