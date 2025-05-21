from test import TestCase, generate_random_vector
import struct
import json
import random

class VSIMWithAttribs(TestCase):
    def getname(self):
        return "VSIM WITHATTRIBS/WITHSCORES functionality testing"

    def setup(self):
        super().setup()
        self.dim = 8
        self.count = 20

        # Create vectors with attributes
        for i in range(self.count):
            vec = generate_random_vector(self.dim)
            vec_bytes = struct.pack(f'{self.dim}f', *vec)

            # Item name
            name = f"{self.test_key}:item:{i}"

            # Add to Redis
            self.redis.execute_command('VADD', self.test_key, 'FP32', vec_bytes, name)

            # Create and add attribute
            if i % 5 == 0:
                # Every 5th item has no attribute (for testing NULL responses)
                continue

            category = random.choice(["electronics", "furniture", "clothing"])
            price = random.randint(50, 1000)
            attrs = {"category": category, "price": price, "id": i}

            self.redis.execute_command('VSETATTR', self.test_key, name, json.dumps(attrs))

    def is_numeric_string(self, value):
        """Check if a value is a string/bytes that represents a number"""
        if isinstance(value, bytes):
            value = value.decode('utf-8')
        try:
            float(value)
            return True
        except (ValueError, TypeError):
            return False

    def test(self):
        # Create query vector
        query_vec = generate_random_vector(self.dim)

        # Test 1: VSIM with no additional options
        cmd_args = ['VSIM', self.test_key, 'VALUES', self.dim]
        cmd_args.extend([str(x) for x in query_vec])
        cmd_args.extend(['COUNT', 5])

        results = self.redis.execute_command(*cmd_args)

        # Results should be a simple array of item names
        assert len(results) == 5, f"Expected 5 results, got {len(results)}"
        assert all(isinstance(item, bytes) for item in results), "Results should be byte strings"

        # Test 2: VSIM with WITHSCORES only
        cmd_args = ['VSIM', self.test_key, 'VALUES', self.dim]
        cmd_args.extend([str(x) for x in query_vec])
        cmd_args.extend(['COUNT', 5, 'WITHSCORES'])

        results = self.redis.execute_command(*cmd_args)

        # Results should be alternating item, score pairs
        assert len(results) == 10, f"Expected 10 results (5 items with scores), got {len(results)}"
        for i in range(0, len(results), 2):
            assert isinstance(results[i], bytes), f"Item at index {i} should be a byte string"
            # Check if the score can be parsed as a float (Redis returns scores as strings)
            assert self.is_numeric_string(results[i+1]), f"Score at index {i+1} should be a numeric string, got {type(results[i+1])}: {results[i+1]}"

            # Additionally, verify the score is in the expected range (0-1 for cosine similarity)
            score_value = float(results[i+1]) if isinstance(results[i+1], bytes) else float(results[i+1])
            assert 0 <= score_value <= 1, f"Score {score_value} should be between 0 and 1"

        # Test 3: VSIM with WITHATTRIBS only
        cmd_args = ['VSIM', self.test_key, 'VALUES', self.dim]
        cmd_args.extend([str(x) for x in query_vec])
        cmd_args.extend(['COUNT', 5, 'WITHATTRIBS'])

        results = self.redis.execute_command(*cmd_args)

        # Results should be alternating item, attribute pairs
        assert len(results) == 10, f"Expected 10 results (5 items with attributes), got {len(results)}"
        for i in range(0, len(results), 2):
            assert isinstance(results[i], bytes), f"Item at index {i} should be a byte string"

            # Attribute can be JSON string or None for items without attributes
            attr = results[i+1]
            assert attr is None or isinstance(attr, bytes), f"Attribute at {i+1} should be None or bytes"

            # If it's a byte string, it should be valid JSON
            if attr is not None:
                try:
                    attr_json = json.loads(attr)
                    assert isinstance(attr_json, dict), "Attribute should deserialize to a dictionary"
                    assert "category" in attr_json, "Attribute should have a category field"
                    assert "price" in attr_json, "Attribute should have a price field"
                except json.JSONDecodeError:
                    assert False, f"Attribute at index {i+1} is not valid JSON: {attr}"

        # Test 4: VSIM with both WITHSCORES and WITHATTRIBS
        cmd_args = ['VSIM', self.test_key, 'VALUES', self.dim]
        cmd_args.extend([str(x) for x in query_vec])
        cmd_args.extend(['COUNT', 5, 'WITHSCORES', 'WITHATTRIBS'])

        results = self.redis.execute_command(*cmd_args)

        # Results format differs depending on RESP version
        resp3 = True if hasattr(self.redis, 'connection_pool') and hasattr(self.redis.connection_pool, 'response_callbacks') else False

        if resp3:
            # For RESP3, with both options we expect a map with array values
            assert len(results) == 5, f"Expected 5 map entries, got {len(results)}"
            # This is a rough check as the exact format depends on the Redis client
        else:
            # For RESP2, with both options we expect a flat structure with 3 elements per item
            assert len(results) == 15, f"Expected 15 elements (5 items × 3), got {len(results)}"

            # Check pattern: item, score, attribute
            for i in range(0, len(results), 3):
                assert isinstance(results[i], bytes), f"Item at index {i} should be a byte string"
                assert self.is_numeric_string(results[i+1]), f"Score at index {i+1} should be a numeric string"

                # Attribute can be JSON string or None
                attr = results[i+2]
                assert attr is None or isinstance(attr, bytes), f"Attribute at {i+2} should be None or bytes"

        # Test 5: Check attributes correctly correspond to items
        cmd_args = ['VSIM', self.test_key, 'VALUES', self.dim]
        cmd_args.extend([str(x) for x in query_vec])
        cmd_args.extend(['COUNT', 5, 'WITHATTRIBS'])

        results = self.redis.execute_command(*cmd_args)

        # For each item, verify the attribute by directly checking with VGETATTR
        for i in range(0, len(results), 2):
            item_name = results[i].decode('utf-8')
            returned_attr = results[i+1]

            # Get the attribute directly
            direct_attr = self.redis.execute_command('VGETATTR', self.test_key, item_name)

            # Verify they match
            if direct_attr is None:
                assert returned_attr is None, f"Item {item_name} should have NULL attribute"
            else:
                assert returned_attr is not None, f"Item {item_name} should have non-NULL attribute"
                assert direct_attr == returned_attr, f"Attributes for {item_name} don't match"
