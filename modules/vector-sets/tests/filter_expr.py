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

        # Basic equality with numbers
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '.age == 25')
        assert len(result) == 1, "Expected 1 result for age == 25"
        assert result[0].decode() == f'{self.test_key}:item:1', "Expected item:1 for age == 25"

        # Greater than
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '.age > 25')
        assert len(result) == 2, "Expected 2 results for age > 25"

        # Less than or equal
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '.age <= 30')
        assert len(result) == 2, "Expected 2 results for age <= 30"

        # String equality
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '.name == "Alice"')
        assert len(result) == 1, "Expected 1 result for name == Alice"
        assert result[0].decode() == f'{self.test_key}:item:1', "Expected item:1 for name == Alice"

        # String inequality
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '.name != "Alice"')
        assert len(result) == 2, "Expected 2 results for name != Alice"

        # Boolean value
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '.active')
        assert len(result) == 1, "Expected 1 result for .active being true"

        # Logical AND
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '.age > 20 and .age < 30')
        assert len(result) == 1, "Expected 1 result for 20 < age < 30"
        assert result[0].decode() == f'{self.test_key}:item:1', "Expected item:1 for 20 < age < 30"

        # Logical OR
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '.age < 30 or .age > 35')
        assert len(result) == 1, "Expected 1 result for age < 30 or age > 35"

        # Logical NOT
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '!(.age == 25)')
        assert len(result) == 2, "Expected 2 results for NOT(age == 25)"

        # The "in" operator with array
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '.age in [25, 35]')
        assert len(result) == 2, "Expected 2 results for age in [25, 35]"

        # The "in" operator with strings in array
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '.name in ["Alice", "David"]')
        assert len(result) == 1, "Expected 1 result for name in [Alice, David]"

        # The "in" operator for substring matching
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '"lic" in .name')
        assert len(result) == 1, "Expected 1 result for 'lic' in name"
        assert result[0].decode() == f'{self.test_key}:item:1', "Expected item:1 (Alice)"

        # The "in" operator with city substring
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '"ork" in .city')
        assert len(result) == 1, "Expected 1 result for 'ork' in city"
        assert result[0].decode() == f'{self.test_key}:item:1', "Expected item:1 (New York)"

        # The "in" operator with no matches
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '"xyz" in .name')
        assert len(result) == 0, "Expected 0 results for 'xyz' in name"

        # Off-by-one tests - substring at the beginning
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '"Ali" in .name')
        assert len(result) == 1, "Expected 1 result for 'Ali' at beginning of 'Alice'"
        assert result[0].decode() == f'{self.test_key}:item:1', "Expected item:1"

        # Off-by-one tests - substring at the end
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '"ice" in .name')
        assert len(result) == 1, "Expected 1 result for 'ice' at end of 'Alice'"
        assert result[0].decode() == f'{self.test_key}:item:1', "Expected item:1"

        # Off-by-one tests - exact match (entire string)
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '"Alice" in .name')
        assert len(result) == 1, "Expected 1 result for exact match 'Alice' in 'Alice'"
        assert result[0].decode() == f'{self.test_key}:item:1', "Expected item:1"

        # Off-by-one tests - single character
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '"A" in .name')
        assert len(result) == 1, "Expected 1 result for single char 'A' in 'Alice'"

        # Off-by-one tests - empty string (should match all strings)
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '"" in .name')
        assert len(result) == 3, "Expected 3 results for empty string (matches all strings)"

        # Off-by-one tests - non-empty strings are never substrings of ""
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '.name in ""')
        assert len(result) == 0, "Expected 0 results for empty string on the right of IN operator"

        # Off-by-one tests - empty string match empty string.
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '"" in .name && "" in ""')
        assert len(result) == 3, "Expected empty string matching empty string"

        # Arithmetic operations - addition
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '.age + 10 > 40')
        assert len(result) == 1, "Expected 1 result for age + 10 > 40"

        # Arithmetic operations - multiplication
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '.age * 2 > 60')
        assert len(result) == 1, "Expected 1 result for age * 2 > 60"

        # Arithmetic operations - division
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '.age / 5 == 5')
        assert len(result) == 1, "Expected 1 result for age / 5 == 5"

        # Arithmetic operations - modulo
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '.age % 2 == 0')
        assert len(result) == 1, "Expected 1 result for age % 2 == 0"

        # Power operator
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '.age ** 2 > 900')
        assert len(result) == 1, "Expected 1 result for age^2 > 900"

        # Missing attribute (should exclude items missing that attribute)
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '.missing_field == "value"')
        assert len(result) == 0, "Expected 0 results for missing_field == value"

        # No attribute set at all
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '.any_field')
        assert f'{self.test_key}:item:4' not in [item.decode() for item in result], "Item with no attribute should be excluded"

        # Malformed JSON
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '.any_field')
        assert f'{self.test_key}:item:5' not in [item.decode() for item in result], "Item with malformed JSON should be excluded"

        # Complex expression combining multiple operators
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '(.age > 20 and .age < 40) and (.city == "Boston" or .city == "New York")')
        assert len(result) == 2, "Expected 2 results for the complex expression"
        expected_items = [f'{self.test_key}:item:1', f'{self.test_key}:item:2']
        assert set([item.decode() for item in result]) == set(expected_items), "Expected item:1 and item:2 for the complex expression"

        # Parentheses to control operator precedence
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '.age > (20 + 10)')
        assert len(result) == 1, "Expected 1 result for age > (20 + 10)"

        # Array access (arrays evaluate to true)
        result = self.redis.execute_command('VSIM', self.test_key, 'VALUES', 4,
                                          *[str(x) for x in vec1],
                                          'FILTER', '.scores')
        assert len(result) == 3, "Expected 3 results for .scores (arrays evaluate to true)"
