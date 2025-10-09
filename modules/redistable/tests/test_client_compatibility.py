#!/usr/bin/env python3
"""
Redis Table Module - Client Compatibility Test Suite (Python)
Tests that special characters in arguments work correctly with redis-py
"""

import redis
import sys

def test_python_client():
    """Test redis-py client compatibility"""
    print("=" * 60)
    print("Redis Table Module - Python Client Compatibility Test")
    print("=" * 60)
    
    try:
        # Connect to Redis
        r = redis.Redis(host='localhost', port=6379, decode_responses=True)
        r.ping()
        print("✓ Connected to Redis")
    except Exception as e:
        print(f"✗ Failed to connect to Redis: {e}")
        return False
    
    test_namespace = "test_py"
    test_table = f"{test_namespace}.compatibility"
    
    try:
        # Cleanup
        try:
            r.execute_command('TABLE.DROP', test_table, 'FORCE')
        except:
            pass
        
        print(f"\n{'='*60}")
        print("Test 1: Special Characters in Table Names (dots)")
        print(f"{'='*60}")
        
        # Test 1: Create namespace
        result = r.execute_command('TABLE.NAMESPACE.CREATE', test_namespace)
        assert result == 'OK', f"Expected OK, got {result}"
        print(f"✓ PASS: Created namespace with name: {test_namespace}")
        
        # Test 2: Create table with dots in name
        result = r.execute_command('TABLE.SCHEMA.CREATE', test_table,
                                   'NAME:string:true',
                                   'AGE:integer:false',
                                   'EMAIL:string:true')
        assert result == 'OK', f"Expected OK, got {result}"
        print(f"✓ PASS: Created table with dot in name: {test_table}")
        
        print(f"\n{'='*60}")
        print("Test 2: Special Characters in Column Definitions (colons)")
        print(f"{'='*60}")
        
        # Verify schema contains colons
        schema = r.execute_command('TABLE.SCHEMA.VIEW', test_table)
        assert 'NAME' in str(schema), "NAME column not found"
        assert 'string' in str(schema), "string type not found"
        print(f"✓ PASS: Column definitions with colons parsed correctly")
        print(f"  Schema: {schema}")
        
        print(f"\n{'='*60}")
        print("Test 3: Special Characters in Insert (equals signs)")
        print(f"{'='*60}")
        
        # Test 3: Insert with equals signs
        result = r.execute_command('TABLE.INSERT', test_table,
                                   'NAME=John Doe',
                                   'AGE=30',
                                   'EMAIL=john@example.com')
        assert result, "Insert failed"
        print(f"✓ PASS: Inserted data with equals signs: NAME=John Doe")
        print(f"  Returned ID: {result}")
        
        print(f"\n{'='*60}")
        print("Test 4: Special Characters in WHERE (operators)")
        print(f"{'='*60}")
        
        # Test 4: Query with operators
        results = r.execute_command('TABLE.SELECT', test_table,
                                   'WHERE', 'AGE>25')
        assert len(results) > 0, "No results found"
        print(f"✓ PASS: Query with > operator worked")
        print(f"  Results: {results}")
        
        # Test 5: Query with = operator
        results = r.execute_command('TABLE.SELECT', test_table,
                                   'WHERE', 'NAME=John Doe')
        assert len(results) > 0, "No results with equality"
        print(f"✓ PASS: Query with = operator worked")
        
        print(f"\n{'='*60}")
        print("Test 5: Special Characters in Complex Queries (AND/OR)")
        print(f"{'='*60}")
        
        # Insert more data
        r.execute_command('TABLE.INSERT', test_table,
                         'NAME=Jane Smith',
                         'AGE=28',
                         'EMAIL=jane@example.com')
        
        # Test 6: Complex query with AND
        results = r.execute_command('TABLE.SELECT', test_table,
                                   'WHERE', 'AGE>25', 'AND', 'NAME=John Doe')
        assert len(results) > 0, "AND query failed"
        print(f"✓ PASS: Complex query with AND operator worked")
        
        print(f"\n{'='*60}")
        print("Test 6: Special Characters in UPDATE (equals in SET)")
        print(f"{'='*60}")
        
        # Test 7: Update with equals
        result = r.execute_command('TABLE.UPDATE', test_table,
                                   'WHERE', 'NAME=John Doe',
                                   'SET', 'AGE=31')
        assert result, "Update failed"
        print(f"✓ PASS: Updated with equals in SET clause")
        
        # Verify update
        results = r.execute_command('TABLE.SELECT', test_table,
                                   'WHERE', 'NAME=John Doe')
        assert 'AGE' in str(results) and '31' in str(results), "Update not reflected"
        print(f"✓ PASS: Update verified in query results")
        
        print(f"\n{'='*60}")
        print("Test 7: Spaces in Values")
        print(f"{'='*60}")
        
        # Test 8: Spaces in values
        r.execute_command('TABLE.INSERT', test_table,
                         'NAME=Bob Johnson Jr.',
                         'AGE=45',
                         'EMAIL=bob.jr@example.com')
        
        results = r.execute_command('TABLE.SELECT', test_table,
                                   'WHERE', 'NAME=Bob Johnson Jr.')
        assert len(results) > 0, "Spaces in values failed"
        print(f"✓ PASS: Values with spaces handled correctly")
        print(f"  Name: 'Bob Johnson Jr.'")
        
        print(f"\n{'='*60}")
        print("Test 8: Special Characters in Email (@ symbol)")
        print(f"{'='*60}")
        
        results = r.execute_command('TABLE.SELECT', test_table,
                                   'WHERE', 'EMAIL=john@example.com')
        assert len(results) > 0, "@ symbol in value failed"
        print(f"✓ PASS: Email with @ symbol handled correctly")
        
        # Cleanup
        r.execute_command('TABLE.DROP', test_table, 'FORCE')
        print(f"\n✓ PASS: Cleanup successful")
        
        print(f"\n{'='*60}")
        print("ALL TESTS PASSED - Python client is fully compatible!")
        print(f"{'='*60}")
        return True
        
    except AssertionError as e:
        print(f"\n✗ FAIL: {e}")
        return False
    except Exception as e:
        print(f"\n✗ ERROR: {e}")
        import traceback
        traceback.print_exc()
        return False
    finally:
        # Final cleanup
        try:
            r.execute_command('TABLE.DROP', test_table, 'FORCE')
        except:
            pass

if __name__ == '__main__':
    success = test_python_client()
    sys.exit(0 if success else 1)
