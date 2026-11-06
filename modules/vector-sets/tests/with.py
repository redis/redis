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

    def is_numeric(self, value):
        """Check if a value can be converted to float"""
        try:
            if isinstance(value, (int, float)):
                return True
            if isinstance(value, bytes):
                float(value.decode('utf-8'))
                return True
            if isinstance(value, str):
                float(value)
                return True
            return False
        except (ValueError, TypeError):
            return False

    def test(self):
        # Create query vector
        query_vec = generate_random_vector(self.dim)

        # Test 1: VSIM with no additional options (should be same for RESP2 and RESP3)
        cmd_args = ['VSIM', self.test_key, 'VALUES', self.dim]
        cmd_args.extend([str(x) for x in query_vec])
        cmd_args.extend(['COUNT', 5])

        results_resp2 = self.redis.execute_command(*cmd_args)
        results_resp3 = self.redis3.execute_command(*cmd_args)

        # Both should return simple arrays of item names
        assert len(results_resp2) == 5, f"RESP2: Expected 5 results, got {len(results_resp2)}"
        assert len(results_resp3) == 5, f"RESP3: Expected 5 results, got {len(results_resp3)}"
        assert all(isinstance(item, bytes) for item in results_resp2), "RESP2: Results should be byte strings"
        assert all(isinstance(item, bytes) for item in results_resp3), "RESP3: Results should be byte strings"

        # Test 2: VSIM with WITHSCORES only
        cmd_args = ['VSIM', self.test_key, 'VALUES', self.dim]
        cmd_args.extend([str(x) for x in query_vec])
        cmd_args.extend(['COUNT', 5, 'WITHSCORES'])

        results_resp2 = self.redis.execute_command(*cmd_args)
        results_resp3 = self.redis3.execute_command(*cmd_args)

        # RESP2: Should be a flat array alternating item, score
        assert len(results_resp2) == 10, f"RESP2: Expected 10 elements (5 items × 2), got {len(results_resp2)}"
        for i in range(0, len(results_resp2), 2):
            assert isinstance(results_resp2[i], bytes), f"RESP2: Item at {i} should be bytes"
            assert self.is_numeric(results_resp2[i+1]), f"RESP2: Score at {i+1} should be numeric"
            score = float(results_resp2[i+1]) if isinstance(results_resp2[i+1], bytes) else results_resp2[i+1]
            assert 0 <= score <= 1, f"RESP2: Score {score} should be between 0 and 1"

        # RESP3: Should be a dict/map with items as keys and scores as DIRECT values (not arrays)
        assert isinstance(results_resp3, dict), f"RESP3: Expected dict, got {type(results_resp3)}"
        assert len(results_resp3) == 5, f"RESP3: Expected 5 entries, got {len(results_resp3)}"
        for item, score in results_resp3.items():
            assert isinstance(item, bytes), f"RESP3: Key should be bytes"
            # Score should be a direct value, NOT an array
            assert not isinstance(score, list), f"RESP3: With single WITH option, value should not be array"
            assert self.is_numeric(score), f"RESP3: Score should be numeric, got {type(score)}"
            score_val = float(score) if isinstance(score, bytes) else score
            assert 0 <= score_val <= 1, f"RESP3: Score {score_val} should be between 0 and 1"

        # Test 3: VSIM with WITHATTRIBS only
        cmd_args = ['VSIM', self.test_key, 'VALUES', self.dim]
        cmd_args.extend([str(x) for x in query_vec])
        cmd_args.extend(['COUNT', 5, 'WITHATTRIBS'])

        results_resp2 = self.redis.execute_command(*cmd_args)
        results_resp3 = self.redis3.execute_command(*cmd_args)

        # RESP2: Should be a flat array alternating item, attribute
        assert len(results_resp2) == 10, f"RESP2: Expected 10 elements (5 items × 2), got {len(results_resp2)}"
        for i in range(0, len(results_resp2), 2):
            assert isinstance(results_resp2[i], bytes), f"RESP2: Item at {i} should be bytes"
            attr = results_resp2[i+1]
            assert attr is None or isinstance(attr, bytes), f"RESP2: Attribute at {i+1} should be None or bytes"
            if attr is not None:
                # Verify it's valid JSON
                json.loads(attr)

        # RESP3: Should be a dict/map with items as keys and attributes as DIRECT values (not arrays)
        assert isinstance(results_resp3, dict), f"RESP3: Expected dict, got {type(results_resp3)}"
        assert len(results_resp3) == 5, f"RESP3: Expected 5 entries, got {len(results_resp3)}"
        for item, attr in results_resp3.items():
            assert isinstance(item, bytes), f"RESP3: Key should be bytes"
            # Attribute should be a direct value, NOT an array
            assert not isinstance(attr, list), f"RESP3: With single WITH option, value should not be array"
            assert attr is None or isinstance(attr, bytes), f"RESP3: Attribute should be None or bytes"
            if attr is not None:
                # Verify it's valid JSON
                json.loads(attr)

        # Test 4: VSIM with both WITHSCORES and WITHATTRIBS
        cmd_args = ['VSIM', self.test_key, 'VALUES', self.dim]
        cmd_args.extend([str(x) for x in query_vec])
        cmd_args.extend(['COUNT', 5, 'WITHSCORES', 'WITHATTRIBS'])

        results_resp2 = self.redis.execute_command(*cmd_args)
        results_resp3 = self.redis3.execute_command(*cmd_args)

        # RESP2: Should be a flat array with pattern: item, score, attribute
        assert len(results_resp2) == 15, f"RESP2: Expected 15 elements (5 items × 3), got {len(results_resp2)}"
        for i in range(0, len(results_resp2), 3):
            assert isinstance(results_resp2[i], bytes), f"RESP2: Item at {i} should be bytes"
            assert self.is_numeric(results_resp2[i+1]), f"RESP2: Score at {i+1} should be numeric"
            score = float(results_resp2[i+1]) if isinstance(results_resp2[i+1], bytes) else results_resp2[i+1]
            assert 0 <= score <= 1, f"RESP2: Score {score} should be between 0 and 1"
            attr = results_resp2[i+2]
            assert attr is None or isinstance(attr, bytes), f"RESP2: Attribute at {i+2} should be None or bytes"

        # RESP3: Should be a dict where each value is a 2-element array [score, attribute]
        assert isinstance(results_resp3, dict), f"RESP3: Expected dict, got {type(results_resp3)}"
        assert len(results_resp3) == 5, f"RESP3: Expected 5 entries, got {len(results_resp3)}"
        for item, value in results_resp3.items():
            assert isinstance(item, bytes), f"RESP3: Key should be bytes"
            # With BOTH options, value MUST be an array
            assert isinstance(value, list), f"RESP3: With both WITH options, value should be a list, got {type(value)}"
            assert len(value) == 2, f"RESP3: Value should have 2 elements [score, attr], got {len(value)}"

            score, attr = value
            assert self.is_numeric(score), f"RESP3: Score should be numeric"
            score_val = float(score) if isinstance(score, bytes) else score
            assert 0 <= score_val <= 1, f"RESP3: Score {score_val} should be between 0 and 1"
            assert attr is None or isinstance(attr, bytes), f"RESP3: Attribute should be None or bytes"

        # Test 5: Verify consistency - same items returned in same order
        cmd_args = ['VSIM', self.test_key, 'VALUES', self.dim]
        cmd_args.extend([str(x) for x in query_vec])
        cmd_args.extend(['COUNT', 5, 'WITHSCORES', 'WITHATTRIBS'])

        results_resp2 = self.redis.execute_command(*cmd_args)
        results_resp3 = self.redis3.execute_command(*cmd_args)

        # Extract items from RESP2 (every 3rd element starting from 0)
        items_resp2 = [results_resp2[i] for i in range(0, len(results_resp2), 3)]

        # Extract items from RESP3 (keys of the dict)
        items_resp3 = list(results_resp3.keys())

        # Verify same items returned
        assert set(items_resp2) == set(items_resp3), "RESP2 and RESP3 should return the same items"

        # Build a mapping from items to scores and attributes for comparison
        data_resp2 = {}
        for i in range(0, len(results_resp2), 3):
            item = results_resp2[i]
            score = float(results_resp2[i+1]) if isinstance(results_resp2[i+1], bytes) else results_resp2[i+1]
            attr = results_resp2[i+2]
            data_resp2[item] = (score, attr)

        data_resp3 = {}
        for item, value in results_resp3.items():
            score = float(value[0]) if isinstance(value[0], bytes) else value[0]
            attr = value[1]
            data_resp3[item] = (score, attr)

        # Verify scores and attributes match for each item
        for item in data_resp2:
            score_resp2, attr_resp2 = data_resp2[item]
            score_resp3, attr_resp3 = data_resp3[item]

            assert abs(score_resp2 - score_resp3) < 0.0001, \
                f"Scores for {item} don't match: RESP2={score_resp2}, RESP3={score_resp3}"
            assert attr_resp2 == attr_resp3, \
                f"Attributes for {item} don't match: RESP2={attr_resp2}, RESP3={attr_resp3}"

        # Test 6: Test ordering of WITHSCORES and WITHATTRIBS doesn't matter
        cmd_args1 = ['VSIM', self.test_key, 'VALUES', self.dim]
        cmd_args1.extend([str(x) for x in query_vec])
        cmd_args1.extend(['COUNT', 3, 'WITHSCORES', 'WITHATTRIBS'])

        cmd_args2 = ['VSIM', self.test_key, 'VALUES', self.dim]
        cmd_args2.extend([str(x) for x in query_vec])
        cmd_args2.extend(['COUNT', 3, 'WITHATTRIBS', 'WITHSCORES'])  # Reversed order

        results1_resp3 = self.redis3.execute_command(*cmd_args1)
        results2_resp3 = self.redis3.execute_command(*cmd_args2)

        # Both should return the same structure
        assert results1_resp3 == results2_resp3, "Order of WITH options shouldn't matter"
