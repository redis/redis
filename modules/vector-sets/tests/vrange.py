from test import TestCase, generate_random_vector
import struct

class BasicVRANGE(TestCase):
    def getname(self):
        return "VRANGE basic functionality and iteration"

    def test(self):
        # Add multiple elements with different names for lexicographical ordering
        elements = [
            "apple", "apricot", "banana", "cherry", "date",
            "elderberry", "fig", "grape", "honeydew", "kiwi",
            "lemon", "mango", "nectarine", "orange", "papaya",
            "quince", "raspberry", "strawberry", "tangerine", "watermelon"
        ]

        # Add all elements to the vector set
        for elem in elements:
            vec = generate_random_vector(4)
            vec_bytes = struct.pack('4f', *vec)
            self.redis.execute_command('VADD', self.test_key, 'FP32', vec_bytes, elem)

        # Test 1: Basic range with inclusive boundaries
        result = self.redis.execute_command('VRANGE', self.test_key, '[apple', '[grape', '5')
        result = [r.decode() for r in result]
        assert result == ['apple', 'apricot', 'banana', 'cherry', 'date'], f"Expected first 5 elements from apple, got {result}"

        # Test 2: Exclusive start boundary
        result = self.redis.execute_command('VRANGE', self.test_key, '(apple', '[cherry', '10')
        result = [r.decode() for r in result]
        assert result == ['apricot', 'banana', 'cherry'], f"Expected elements after apple up to cherry inclusive, got {result}"

        # Test 3: Exclusive end boundary
        result = self.redis.execute_command('VRANGE', self.test_key, '[banana', '(cherry', '10')
        result = [r.decode() for r in result]
        assert result == ['banana'], f"Expected only banana (cherry excluded), got {result}"

        # Test 4: Using '-' for minimum element
        result = self.redis.execute_command('VRANGE', self.test_key, '-', '[banana', '10')
        result = [r.decode() for r in result]
        assert result[0] == 'apple', "Should start from the first element"
        assert result[-1] == 'banana', "Should end at banana"

        # Test 5: Using '+' for maximum element
        result = self.redis.execute_command('VRANGE', self.test_key, '[raspberry', '+', '10')
        result = [r.decode() for r in result]
        assert 'raspberry' in result and 'strawberry' in result and 'tangerine' in result and 'watermelon' in result, "Should include all elements from raspberry onwards"

        # Test 6: Full range with '-' and '+'
        result = self.redis.execute_command('VRANGE', self.test_key, '-', '+', '100')
        result = [r.decode() for r in result]
        assert len(result) == len(elements), f"Should return all {len(elements)} elements"
        assert result == sorted(elements), "Elements should be in lexicographical order"

        # Test 7: Iterator pattern - verify each element appears exactly once
        seen = set()
        batch_size = 3
        current = '-'

        while True:
            if current == '-':
                # First iteration
                result = self.redis.execute_command('VRANGE', self.test_key, '-', '+', str(batch_size))
            else:
                # Subsequent iterations - exclusive start from last element
                result = self.redis.execute_command('VRANGE', self.test_key, f'({current}', '+', str(batch_size))

            result = [r.decode() for r in result]

            if not result:
                break

            # Check no duplicates in this batch
            for elem in result:
                assert elem not in seen, f"Element {elem} appeared more than once"
                seen.add(elem)

            # Update current to last element
            current = result[-1]

            # Break if we got less than requested (end of set)
            if len(result) < batch_size:
                break

        # Verify we saw all elements exactly once
        assert seen == set(elements), f"Iterator should visit all elements exactly once. Missing: {set(elements) - seen}, Extra: {seen - set(elements)}"

        # Test 8: Count of 0 returns empty array
        result = self.redis.execute_command('VRANGE', self.test_key, '-', '+', '0')
        assert result == [], f"Count of 0 should return empty array, got {result}"

        # Test 9: Range with no matching elements
        result = self.redis.execute_command('VRANGE', self.test_key, '[zebra', '+', '10')
        assert result == [], f"Range beyond all elements should return empty array, got {result}"

        # Test 10: Non-existent key
        result = self.redis.execute_command('VRANGE', 'nonexistent_key', '-', '+', '10')
        assert result == [], f"Non-existent key should return empty array, got {result}"

        # Test 11: Partial word boundaries
        result = self.redis.execute_command('VRANGE', self.test_key, '[app', '[apr', '10')
        result = [r.decode() for r in result]
        assert 'apple' in result, "Should include 'apple' which starts with 'app'"
        assert 'apricot' not in result, "Should not include 'apricot' as it's >= 'apr'"

        # Test 12: Single element range
        result = self.redis.execute_command('VRANGE', self.test_key, '[cherry', '[cherry', '10')
        result = [r.decode() for r in result]
        assert result == ['cherry'], f"Inclusive single element range should return that element, got {result}"

        # Test 13: Empty range (start > end)
        result = self.redis.execute_command('VRANGE', self.test_key, '[grape', '[apple', '10')
        assert result == [], f"Range where start > end should return empty array, got {result}"
