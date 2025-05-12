from test import TestCase

class VSIMFilterExpressions(TestCase):
    def getname(self):
        return "VSIM FILTER expressions basic functionality"

    def test(self):
        # Create a small set of vectors with different attributes

        # Basic vectors for testing - all orthogonal for clear results
        vec1 = [1, 0, 0, 0]
        vec2 = [0, 1, 0, 0]
        vec3 = [0, 0, 1, 0]
        vec4 = [0, 0, 0, 1]
        vec5 = [0.5, 0.5, 0, 0]

        # Add vectors with various attributes
        self.redis.execute_command('VADD', self.test_key, 'VALUES', 4,
                                 *[str(x) for x in vec1], f'{self.test_key}:item:1')
        self.redis.execute_command('VSETATTR', self.test_key, f'{self.test_key}:item:1',
                                  '{"age": 25, "name": "Alice", "active": true, "scores": [85, 90, 95], "city": "New York"}')

        self.redis.execute_command('VADD', self.test_key, 'VALUES', 4,
                                 *[str(x) for x in vec2], f'{self.test_key}:item:2')
        self.redis.execute_command('VSETATTR', self.test_key, f'{self.test_key}:item:2',
                                  '{"age": 30, "name": "Bob", "active": false, "scores": [70, 75, 80], "city": "Boston"}')

        self.redis.execute_command('VADD', self.test_key, 'VALUES', 4,
                                 *[str(x) for x in vec3], f'{self.test_key}:item:3')
        self.redis.execute_command('VSETATTR', self.test_key, f'{self.test_key}:item:3',
                                  '{"age": 35, "name": "Charlie", "scores": [60, 65, 70], "city": "Seattle"}')

        self.redis.execute_command('VADD', self.test_key, 'VALUES', 4,
                                 *[str(x) for x in vec4], f'{self.test_key}:item:4')
        # Item 4 has no attribute at all

        self.redis.execute_command('VADD', self.test_key, 'VALUES', 4,
                                 *[str(x) for x in vec5], f'{self.test_key}:item:5')
        self.redis.execute_command('VSETATTR', self.test_key, f'{self.test_key}:item:5',
                                  'invalid json')  # Intentionally malformed JSON

        # Test 1: Basic equality with numbers
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '.age == 25')
        assert len(result) == 1, "Expected 1 result for age == 25"
        assert result[0].decode() == f'{self.test_key}:item:1', "Expected item:1 for age == 25"

        # Test 2: Greater than
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '.age > 25')
        assert len(result) == 2, "Expected 2 results for age > 25"

        # Test 3: Less than or equal
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '.age <= 30')
        assert len(result) == 2, "Expected 2 results for age <= 30"

        # Test 4: String equality
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '.name == "Alice"')
        assert len(result) == 1, "Expected 1 result for name == Alice"
        assert result[0].decode() == f'{self.test_key}:item:1', "Expected item:1 for name == Alice"

        # Test 5: String inequality
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '.name != "Alice"')
        assert len(result) == 2, "Expected 2 results for name != Alice"

        # Test 6: Boolean value
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '.active')
        assert len(result) == 1, "Expected 1 result for .active being true"

        # Test 7: Logical AND
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '.age > 20 and .age < 30')
        assert len(result) == 1, "Expected 1 result for 20 < age < 30"
        assert result[0].decode() == f'{self.test_key}:item:1', "Expected item:1 for 20 < age < 30"

        # Test 8: Logical OR
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '.age < 30 or .age > 35')
        assert len(result) == 1, "Expected 1 result for age < 30 or age > 35"

        # Test 9: Logical NOT
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '!(.age == 25)')
        assert len(result) == 2, "Expected 2 results for NOT(age == 25)"

        # Test 10: The "in" operator with array
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '.age in [25, 35]')
        assert len(result) == 2, "Expected 2 results for age in [25, 35]"

        # Test 11: The "in" operator with strings in array
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '.name in ["Alice", "David"]')
        assert len(result) == 1, "Expected 1 result for name in [Alice, David]"

        # Test 12: Arithmetic operations - addition
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '.age + 10 > 40')
        assert len(result) == 1, "Expected 1 result for age + 10 > 40"

        # Test 13: Arithmetic operations - multiplication
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '.age * 2 > 60')
        assert len(result) == 1, "Expected 1 result for age * 2 > 60"

        # Test 14: Arithmetic operations - division
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '.age / 5 == 5')
        assert len(result) == 1, "Expected 1 result for age / 5 == 5"

        # Test 15: Arithmetic operations - modulo
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '.age % 2 == 0')
        assert len(result) == 1, "Expected 1 result for age % 2 == 0"

        # Test 16: Power operator
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '.age ** 2 > 900')
        assert len(result) == 1, "Expected 1 result for age^2 > 900"

        # Test 17: Missing attribute (should exclude items missing that attribute)
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '.missing_field == "value"')
        assert len(result) == 0, "Expected 0 results for missing_field == value"

        # Test 18: No attribute set at all
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '.any_field')
        assert f'{self.test_key}:item:4' not in [item.decode() for item in result], "Item with no attribute should be excluded"

        # Test 19: Malformed JSON
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '.any_field')
        assert f'{self.test_key}:item:5' not in [item.decode() for item in result], "Item with malformed JSON should be excluded"

        # Test 20: Complex expression combining multiple operators
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '(.age > 20 and .age < 40) and (.city == "Boston" or .city == "New York")')
        assert len(result) == 2, "Expected 2 results for the complex expression"
        expected_items = [f'{self.test_key}:item:1', f'{self.test_key}:item:2']
        assert set([item.decode() for item in result]) == set(expected_items), "Expected item:1 and item:2 for the complex expression"

        # Test 21: Parentheses to control operator precedence
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '.age > (20 + 10)')
        assert len(result) == 1, "Expected 1 result for age > (20 + 10)"

        # Test 22: Array access (arrays evaluate to true)
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '.scores')
        assert len(result) == 3, "Expected 3 results for .scores (arrays evaluate to true)"
