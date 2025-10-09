# Client Compatibility Test Suite

## Overview

This test suite verifies that the Redis Table Module works correctly with popular Redis clients in multiple programming languages, especially with special characters in command arguments.

---

## Test Coverage

### What We Test

All tests verify that **special characters in arguments** are handled correctly:

1. **Dots in table names**: `namespace.table`
2. **Colons in column definitions**: `NAME:string:true`
3. **Equals in key-value pairs**: `NAME=John`
4. **Operators in conditions**: `AGE>30`, `SALARY>=50000`
5. **Logical operators**: `AND`, `OR`
6. **Spaces in values**: `NAME=John Doe`
7. **Special chars in values**: `EMAIL=user@example.com`
8. **Complex queries**: `WHERE AGE>25 AND DEPT=Engineering`

### Supported Languages

| Language | Client | Test File | Status |
|----------|--------|-----------|--------|
| **Python** | redis-py | `test_client_compatibility.py` | ✅ Complete |
| **Node.js** | node-redis | `test_client_compatibility.js` | ✅ Complete |
| **Go** | go-redis | _To be added_ | 📋 Planned |
| **Java** | Jedis | _To be added_ | 📋 Planned |
| **C#** | StackExchange.Redis | _To be added_ | 📋 Planned |

---

## Running Tests

### Quick Start

```bash
# Run client tests (will skip if dependencies missing)
make test-clients

# Install dependencies to run tests

# Python option 1: System package (recommended for Ubuntu/Debian)
sudo apt install python3-redis

# Python option 2: Virtual environment (if system package not available)
python3 -m venv venv
source venv/bin/activate
pip install redis

# Node.js tests (requires Node.js installed)
npm install redis

# Or run directly
cd tests
./run_client_tests.sh
```

**Note**: Client tests are **optional**. If dependencies are not installed, tests will be skipped and the command will exit successfully. The core module tests (`make test`) do not require these dependencies.

**Python Installation Note**: Modern Ubuntu/Debian systems use PEP 668 externally-managed environments. Use system packages (`apt install python3-redis`) or virtual environments instead of global pip install.

### Run Individual Tests

**Python**:
```bash
cd tests
python3 test_client_compatibility.py
```

**Node.js**:
```bash
cd tests
node test_client_compatibility.js
```

---

## Test Architecture

### How It Works

1. **Start Redis** with table module loaded
2. **Run language-specific tests** that:
   - Create namespaces/tables with special chars
   - Insert data with special chars
   - Query with operators and special chars
   - Update/delete with special chars
   - Verify all results are correct
3. **Clean up** test data
4. **Report results**

### Test Structure

Each test file follows the same pattern:

```
1. Connect to Redis
2. Create test namespace (e.g., test_py)
3. Create test table (e.g., test_py.compatibility)
4. Test special characters:
   - Dots in names
   - Colons in definitions
   - Equals in inserts
   - Operators in queries
   - Spaces in values
   - Complex queries
5. Cleanup
6. Report pass/fail
```

---

## Adding New Language Tests

To add tests for a new language:

1. **Create test file**: `test_client_compatibility.<ext>`
2. **Follow the pattern**:
   ```
   - Connect to Redis
   - Test all special character scenarios
   - Verify results
   - Clean up
   - Report success/failure
   ```
3. **Update run_client_tests.sh**: Add detection and execution
4. **Update this README**: Add language to supported list

**Template structure**:
```python
def test_<language>_client():
    # Setup
    connect_to_redis()
    namespace = "test_<lang>"
    
    # Test 1: Dots in table names
    create_table(f"{namespace}.compatibility")
    
    # Test 2: Colons in column definitions
    verify_schema_contains_colons()
    
    # Test 3: Equals in inserts
    insert_data(NAME=value, AGE=value)
    
    # Test 4: Operators in queries
    query_with_operators(AGE>25)
    
    # Test 5: Complex queries
    query_with_AND_OR()
    
    # Cleanup
    drop_table()
    
    # Report
    return success
```

---

## Test Results

### Expected Output

```
========================================
Redis Table Module - Client Tests
========================================

✓ Redis server started

========================================
Test 1: Python (redis-py) Client
========================================
✓ Connected to Redis
✓ PASS: Created namespace with name: test_py
✓ PASS: Created table with dot in name: test_py.compatibility
✓ PASS: Column definitions with colons parsed correctly
✓ PASS: Inserted data with equals signs: NAME=John Doe
✓ PASS: Query with > operator worked
✓ PASS: Query with = operator worked
✓ PASS: Complex query with AND operator worked
✓ PASS: Updated with equals in SET clause
✓ PASS: Values with spaces handled correctly
✓ PASS: Email with @ symbol handled correctly
✓ PASS: Cleanup successful

ALL TESTS PASSED - Python client is fully compatible!

========================================
Test 2: Node.js (node-redis) Client
========================================
[Similar output...]

========================================
Client Compatibility Test Summary
========================================
Total Tests Run: 2
Passed:          2
Failed:          0
========================================
✓ All client compatibility tests passed!
```

---

## Why These Tests Are Important

### 1. Validates Documentation Claims

We document that all major clients work with special characters. These tests **prove it**.

### 2. Prevents Regressions

If we change the module code, tests ensure compatibility isn't broken.

### 3. Shows Best Practices

Test code demonstrates **correct** usage patterns for each language.

### 4. Builds Confidence

Users can run tests themselves to verify compatibility before deploying.

---

## Technical Details

### Why Special Characters Work

The Redis Table Module uses the **standard Redis Module API**:

```c
// Module receives arguments as array of RedisModuleString
static int TableInsertCommand(RedisModuleCtx *ctx, 
                             RedisModuleString **argv, 
                             int argc) {
    // argv[0] = "TABLE.INSERT"
    // argv[1] = "mydb.users"       (dot is just part of string)
    // argv[2] = "NAME=John"         (equals is just part of string)
    
    // Extract string content
    const char *s = RedisModule_StringPtrLen(argv[i], &len);
    // Special chars are just regular string content
}
```

**Key insight**: Special characters are **string content**, not protocol delimiters.

Redis clients handle the RESP protocol, which separates arguments. The module receives already-parsed arguments.

### Client Requirements

For compatibility, clients must:
1. ✅ Support `execute_command()` or equivalent for custom commands
2. ✅ Pass arguments as array/list (not concatenated string)
3. ✅ Handle RESP protocol correctly (all modern clients do)

That's it! No special handling needed for our special characters.

---

## Troubleshooting

### Tests Fail to Run

**Problem**: `SKIPPED: redis-py not installed`

**Solution**:
```bash
pip install redis
```

**Problem**: `SKIPPED: Node.js not installed`

**Solution**: Install Node.js from https://nodejs.org/

### Tests Fail with Connection Refused

**Problem**: Redis not running

**Solution**: Test runner starts Redis automatically. If it fails:
```bash
# Check if Redis is already running
ps aux | grep redis-server

# Kill existing instances
pkill redis-server

# Run tests again
./run_client_tests.sh
```

### Test Assertions Fail

**Problem**: Module behavior changed

**Solution**: Review changes to redis_table.c. Tests validate expected behavior.

---

## Future Enhancements

### Planned Additions

- [ ] Go (go-redis) tests
- [ ] Java (Jedis) tests
- [ ] C# (StackExchange.Redis) tests
- [ ] Rust (redis-rs) tests
- [ ] PHP (phpredis) tests
- [ ] Ruby (redis-rb) tests

### Test Improvements

- [ ] Performance benchmarks
- [ ] Concurrency tests
- [ ] Error handling tests
- [ ] Edge case validation

---

## Contributing

To contribute new language tests:

1. Create test file following the pattern
2. Ensure all 8 test scenarios covered
3. Update run_client_tests.sh
4. Update this README
5. Submit pull request

---

**Version**: 2.1  
**Last Updated**: 2025-10-09  
**Test Coverage**: Python, Node.js  
**Status**: Production-ready
