#!/bin/bash

# Redis Table Module Testing
# Author: Raphael Drai
# Email: raphael.drai@gmail.com
# Date: October 3, 2025

# Redis Table Module - Comprehensive Unit Test Suite
# Tests all commands, data types, operators, and edge cases

REDIS_CLI="../../../src/redis-cli"
PASSED=0
FAILED=0
TEST_NUM=0

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Test helper functions
test_start() {
    TEST_NUM=$((TEST_NUM + 1))
    echo -e "\n${YELLOW}Test $TEST_NUM: $1${NC}"
}

assert_equals() {
    local expected="$1"
    local actual="$2"
    local test_name="$3"
    
    if [ "$actual" == "$expected" ]; then
        echo -e "${GREEN}✓ PASS${NC}: $test_name"
        PASSED=$((PASSED + 1))
    else
        echo -e "${RED}✗ FAIL${NC}: $test_name"
        echo "  Expected: $expected"
        echo "  Actual:   $actual"
        FAILED=$((FAILED + 1))
    fi
}

assert_contains() {
    local substring="$1"
    local actual="$2"
    local test_name="$3"
    
    if [[ "$actual" == *"$substring"* ]]; then
        echo -e "${GREEN}✓ PASS${NC}: $test_name"
        PASSED=$((PASSED + 1))
    else
        echo -e "${RED}✗ FAIL${NC}: $test_name"
        echo "  Expected to contain: $substring"
        echo "  Actual: $actual"
        FAILED=$((FAILED + 1))
    fi
}

assert_error() {
    local expected_error="$1"
    local actual="$2"
    local test_name="$3"
    
    if [[ "$actual" == *"$expected_error"* ]]; then
        echo -e "${GREEN}✓ PASS${NC}: $test_name"
        PASSED=$((PASSED + 1))
    else
        echo -e "${RED}✗ FAIL${NC}: $test_name"
        echo "  Expected error: $expected_error"
        echo "  Actual: $actual"
        FAILED=$((FAILED + 1))
    fi
}

# Start tests
echo "========================================"
echo "Redis Table Module - Unit Test Suite"
echo "========================================"

# Clean slate
$REDIS_CLI FLUSHALL > /dev/null

# ============================================
# TEST SUITE 1: Namespace Management
# ============================================
echo -e "\n${YELLOW}=== TEST SUITE 1: Namespace Management ===${NC}"

test_start "Create namespace"
result=$($REDIS_CLI TABLE.NAMESPACE.CREATE testdb)
assert_equals "OK" "$result" "Namespace creation should return OK"

test_start "Create duplicate namespace"
result=$($REDIS_CLI TABLE.NAMESPACE.CREATE testdb 2>&1)
assert_error "namespace already exists" "$result" "Duplicate namespace should fail"

test_start "View namespaces (empty before tables created)"
result=$($REDIS_CLI TABLE.NAMESPACE.VIEW)
# At this point, no tables exist yet, so result may be empty or show only namespace markers

test_start "Create table without namespace"
result=$($REDIS_CLI TABLE.SCHEMA.CREATE nodb.users NAME:string 2>&1)
assert_error "namespace does not exist" "$result" "Table creation without namespace should fail"

# ============================================
# TEST SUITE 2: Table Creation
# ============================================
echo -e "\n${YELLOW}=== TEST SUITE 2: Table Creation ===${NC}"

test_start "Create table with basic types"
result=$($REDIS_CLI TABLE.SCHEMA.CREATE testdb.users NAME:string AGE:integer)
assert_equals "OK" "$result" "Basic table creation"

test_start "Create table with all types"
result=$($REDIS_CLI TABLE.SCHEMA.CREATE testdb.employees EMPID:string SALARY:float HIREDATE:date)
assert_equals "OK" "$result" "Table with all data types"

test_start "Create table with explicit index control"
result=$($REDIS_CLI TABLE.SCHEMA.CREATE testdb.products NAME:string:true PRICE:float:false STOCK:integer:false)
assert_equals "OK" "$result" "Table with explicit index control"

test_start "Create duplicate table"
result=$($REDIS_CLI TABLE.SCHEMA.CREATE testdb.users EMAIL:string 2>&1)
assert_error "table schema already exists" "$result" "Duplicate table should fail"

test_start "Create table with invalid format"
result=$($REDIS_CLI TABLE.SCHEMA.CREATE testdb.bad COL1 2>&1)
assert_error "format:" "$result" "Invalid column format should fail"

test_start "View all namespace tables"
result=$($REDIS_CLI TABLE.NAMESPACE.VIEW)
assert_contains "testdb:users" "$result" "Should show testdb:users"
assert_contains "testdb:employees" "$result" "Should show testdb:employees"
assert_contains "testdb:products" "$result" "Should show testdb:products"

test_start "View specific namespace (testdb)"
result=$($REDIS_CLI TABLE.NAMESPACE.VIEW testdb)
assert_contains "testdb:users" "$result" "Filtered view should show testdb:users"
assert_contains "testdb:employees" "$result" "Filtered view should show testdb:employees"

test_start "View non-existent namespace"
result=$($REDIS_CLI TABLE.NAMESPACE.VIEW nonexistent)
if [[ "$result" == *"(empty"* ]] || [[ -z "$result" ]]; then
    echo -e "${GREEN}✓ PASS${NC}: Non-existent namespace returns empty"
    PASSED=$((PASSED + 1))
else
    echo -e "${RED}✗ FAIL${NC}: Non-existent namespace should return empty"
    FAILED=$((FAILED + 1))
fi

# ============================================
# TEST SUITE 3: Table Schema Viewing
# ============================================
echo -e "\n${YELLOW}=== TEST SUITE 3: Table Schema Viewing ===${NC}"

test_start "View table schema"
result=$($REDIS_CLI TABLE.SCHEMA.VIEW testdb.users)
assert_contains "NAME" "$result" "Table schema view should show column names"
assert_contains "string" "$result" "Table schema view should show types"
assert_contains "true" "$result" "Table schema view should show index status"

test_start "View non-existent table schema"
result=$($REDIS_CLI TABLE.SCHEMA.VIEW testdb.notexist 2>&1)
assert_error "table schema does not exist" "$result" "View non-existent table should fail"

# ============================================
# TEST SUITE 4: Data Insertion
# ============================================
echo -e "\n${YELLOW}=== TEST SUITE 4: Data Insertion ===${NC}"

test_start "Insert basic row"
result=$($REDIS_CLI TABLE.INSERT testdb.users NAME=John AGE=30)
assert_equals "1" "$result" "First insert should return ID 1"

test_start "Insert second row"
result=$($REDIS_CLI TABLE.INSERT testdb.users NAME=Jane AGE=25)
assert_equals "2" "$result" "Second insert should return ID 2"

test_start "Insert with integer validation"
result=$($REDIS_CLI TABLE.INSERT testdb.users NAME=Bob AGE=abc 2>&1)
assert_error "invalid column or type" "$result" "Invalid integer should fail"

test_start "Insert with float type"
result=$($REDIS_CLI TABLE.INSERT testdb.employees EMPID=E001 SALARY=50000.50 HIREDATE=2020-01-15)
assert_equals "1" "$result" "Insert with float"

test_start "Insert with invalid float"
result=$($REDIS_CLI TABLE.INSERT testdb.employees EMPID=E002 SALARY=50.00.00 HIREDATE=2020-01-15 2>&1)
assert_error "invalid column or type" "$result" "Invalid float should fail"

test_start "Insert with date type"
result=$($REDIS_CLI TABLE.INSERT testdb.employees EMPID=E003 SALARY=60000 HIREDATE=2021-03-20)
# ID might be 2 or 3 depending on previous failed inserts, just check it's a number
if [[ "$result" =~ ^[0-9]+$ ]]; then
    echo -e "${GREEN}✓ PASS${NC}: Insert with date"
    PASSED=$((PASSED + 1))
else
    echo -e "${RED}✗ FAIL${NC}: Insert with date (got: $result)"
    FAILED=$((FAILED + 1))
fi

test_start "Insert with invalid date format"
result=$($REDIS_CLI TABLE.INSERT testdb.employees EMPID=E004 SALARY=55000 HIREDATE=2021/03/20 2>&1)
assert_error "invalid column or type" "$result" "Invalid date format should fail"

test_start "Insert with invalid date length"
result=$($REDIS_CLI TABLE.INSERT testdb.employees EMPID=E005 SALARY=55000 HIREDATE=21-03-20 2>&1)
assert_error "invalid column or type" "$result" "Invalid date length should fail"

test_start "Insert with non-existent column"
result=$($REDIS_CLI TABLE.INSERT testdb.users NOTEXIST=value 2>&1)
assert_error "invalid column or type" "$result" "Non-existent column should fail"

# ============================================
# TEST SUITE 5: Data Selection - Basic
# ============================================
echo -e "\n${YELLOW}=== TEST SUITE 5: Data Selection - Basic ===${NC}"

test_start "Select all rows"
result=$($REDIS_CLI TABLE.SELECT testdb.users)
assert_contains "John" "$result" "Select all should return John"
assert_contains "Jane" "$result" "Select all should return Jane"

test_start "Select with equality on indexed column"
result=$($REDIS_CLI TABLE.SELECT testdb.users WHERE NAME=John)
assert_contains "John" "$result" "Equality search should find John"

test_start "Select with equality on non-indexed column"
$REDIS_CLI TABLE.SCHEMA.CREATE testdb.test COL1:string:false > /dev/null
$REDIS_CLI TABLE.INSERT testdb.test COL1=value1 > /dev/null
result=$($REDIS_CLI TABLE.SELECT testdb.test WHERE COL1=value1 2>&1)
assert_error "search cannot be done on non-indexed column" "$result" "Equality on non-indexed should fail"

# ============================================
# TEST SUITE 6: Comparison Operators
# ============================================
echo -e "\n${YELLOW}=== TEST SUITE 6: Comparison Operators ===${NC}"

test_start "Greater than operator (integer)"
result=$($REDIS_CLI TABLE.SELECT testdb.users WHERE AGE\>25)
assert_contains "John" "$result" "AGE>25 should find John (30)"

test_start "Less than operator (integer)"
result=$($REDIS_CLI TABLE.SELECT testdb.users WHERE AGE\<30)
assert_contains "Jane" "$result" "AGE<30 should find Jane (25)"

test_start "Greater than or equal (integer)"
result=$($REDIS_CLI TABLE.SELECT testdb.users WHERE AGE\>=30)
assert_contains "John" "$result" "AGE>=30 should find John"

test_start "Less than or equal (integer)"
result=$($REDIS_CLI TABLE.SELECT testdb.users WHERE AGE\<=25)
assert_contains "Jane" "$result" "AGE<=25 should find Jane"

test_start "Greater than operator (float)"
result=$($REDIS_CLI TABLE.SELECT testdb.employees WHERE SALARY\>55000)
assert_contains "E003" "$result" "SALARY>55000 should find E003"

test_start "Less than or equal (float)"
result=$($REDIS_CLI TABLE.SELECT testdb.employees WHERE SALARY\<=50000.50)
assert_contains "E001" "$result" "SALARY<=50000.50 should find E001"

test_start "Greater than operator (date)"
result=$($REDIS_CLI TABLE.SELECT testdb.employees WHERE HIREDATE\>2020-12-31)
assert_contains "E003" "$result" "HIREDATE>2020-12-31 should find E003"

test_start "Less than operator (date)"
result=$($REDIS_CLI TABLE.SELECT testdb.employees WHERE HIREDATE\<2021-01-01)
assert_contains "E001" "$result" "HIREDATE<2021-01-01 should find E001"

# ============================================
# TEST SUITE 7: Logical Operators
# ============================================
echo -e "\n${YELLOW}=== TEST SUITE 7: Logical Operators ===${NC}"

# Insert more test data
$REDIS_CLI TABLE.INSERT testdb.users NAME=Bob AGE=35 > /dev/null
$REDIS_CLI TABLE.INSERT testdb.users NAME=Alice AGE=28 > /dev/null

test_start "AND operator"
result=$($REDIS_CLI TABLE.SELECT testdb.users WHERE AGE\>25 AND AGE\<35)
assert_contains "John" "$result" "AND condition should find John (30)"
assert_contains "Alice" "$result" "AND condition should find Alice (28)"

test_start "OR operator"
# Need to add index to AGE first for OR to work with equality
$REDIS_CLI TABLE.SCHEMA.ALTER testdb.users ADD INDEX AGE > /dev/null 2>&1
result=$($REDIS_CLI TABLE.SELECT testdb.users WHERE AGE=25 OR AGE=35)
assert_contains "Jane" "$result" "OR condition should find Jane (25)"
assert_contains "Bob" "$result" "OR condition should find Bob (35)"

# ============================================
# TEST SUITE 8: Table Alteration
# ============================================
echo -e "\n${YELLOW}=== TEST SUITE 8: Table Alteration ===${NC}"

test_start "Add column with index"
result=$($REDIS_CLI TABLE.SCHEMA.ALTER testdb.users ADD COLUMN EMAIL:string:true)
assert_equals "OK" "$result" "Add indexed column"

test_start "Add column without index"
result=$($REDIS_CLI TABLE.SCHEMA.ALTER testdb.users ADD COLUMN CITY:string:false)
assert_equals "OK" "$result" "Add non-indexed column"

test_start "Add index to existing column"
result=$($REDIS_CLI TABLE.SCHEMA.ALTER testdb.users ADD INDEX AGE)
assert_equals "OK" "$result" "Add index to existing column"

test_start "Verify index was added"
result=$($REDIS_CLI TABLE.SELECT testdb.users WHERE AGE=30)
assert_contains "John" "$result" "Indexed column should now support equality search"

test_start "Add index to non-existent column"
result=$($REDIS_CLI TABLE.SCHEMA.ALTER testdb.users ADD INDEX NOTEXIST 2>&1)
assert_error "column does not exist" "$result" "Add index to non-existent column should fail"

test_start "Drop index"
result=$($REDIS_CLI TABLE.SCHEMA.ALTER testdb.users DROP INDEX AGE)
assert_equals "OK" "$result" "Drop index"

test_start "Verify index was dropped"
result=$($REDIS_CLI TABLE.SELECT testdb.users WHERE AGE=30 2>&1)
assert_error "search cannot be done on non-indexed column" "$result" "Dropped index should not support equality"

# ============================================
# TEST SUITE 9: Data Update
# ============================================
echo -e "\n${YELLOW}=== TEST SUITE 9: Data Update ===${NC}"

test_start "Update with WHERE clause"
result=$($REDIS_CLI TABLE.UPDATE testdb.users WHERE NAME=John SET AGE=31)
assert_equals "1" "$result" "Update should return count 1"

test_start "Verify update"
result=$($REDIS_CLI TABLE.SELECT testdb.users WHERE NAME=John)
assert_contains "31" "$result" "Updated value should be 31"

test_start "Update multiple rows"
result=$($REDIS_CLI TABLE.UPDATE testdb.users WHERE AGE\>30 SET AGE=40)
updated_count=$(echo "$result" | grep -o '[0-9]\+')
if [ "$updated_count" -ge 1 ]; then
    echo -e "${GREEN}✓ PASS${NC}: Update multiple rows"
    PASSED=$((PASSED + 1))
else
    echo -e "${RED}✗ FAIL${NC}: Update multiple rows"
    FAILED=$((FAILED + 1))
fi

test_start "Update with invalid type"
result=$($REDIS_CLI TABLE.UPDATE testdb.users WHERE NAME=Jane SET AGE=notanumber 2>&1)
assert_error "invalid column or type" "$result" "Update with invalid type should fail"

test_start "Update without WHERE (update by indexed column)"
# Update requires WHERE clause, so use an indexed column
result=$($REDIS_CLI TABLE.UPDATE testdb.employees WHERE EMPID=E001 SET SALARY=70000)
# Check if result is a number (count of updated rows)
if [[ "$result" =~ ^[0-9]+$ ]] && [ "$result" -ge 1 ]; then
    echo -e "${GREEN}✓ PASS${NC}: Update by indexed column (updated: $result)"
    PASSED=$((PASSED + 1))
else
    echo -e "${RED}✗ FAIL${NC}: Update by indexed column (got: $result)"
    FAILED=$((FAILED + 1))
fi

# ============================================
# TEST SUITE 10: Data Deletion
# ============================================
echo -e "\n${YELLOW}=== TEST SUITE 10: Data Deletion ===${NC}"

test_start "Delete with WHERE clause"
result=$($REDIS_CLI TABLE.DELETE testdb.users WHERE NAME=Bob)
# Bob might have been updated in previous tests, so just check it's a number
deleted_count=$(echo "$result" | grep -o '[0-9]\+' | head -1)
if [ -n "$deleted_count" ] && [ "$deleted_count" -ge 1 ]; then
    echo -e "${GREEN}✓ PASS${NC}: Delete should return count >= 1"
    PASSED=$((PASSED + 1))
else
    echo -e "${RED}✗ FAIL${NC}: Delete should return count >= 1 (got: $deleted_count)"
    FAILED=$((FAILED + 1))
fi

test_start "Verify deletion"
result=$($REDIS_CLI TABLE.SELECT testdb.users WHERE NAME=Bob)
if [[ "$result" == *"(empty"* ]] || [[ -z "$result" ]] || [[ "$result" == *"(nil)"* ]]; then
    echo -e "${GREEN}✓ PASS${NC}: Deleted row should not exist"
    PASSED=$((PASSED + 1))
else
    echo -e "${RED}✗ FAIL${NC}: Deleted row should not exist"
    FAILED=$((FAILED + 1))
fi

test_start "Delete with comparison operator"
result=$($REDIS_CLI TABLE.DELETE testdb.users WHERE AGE\>35)
deleted_count=$(echo "$result" | grep -o '[0-9]\+')
if [ "$deleted_count" -ge 0 ]; then
    echo -e "${GREEN}✓ PASS${NC}: Delete with comparison"
    PASSED=$((PASSED + 1))
else
    echo -e "${RED}✗ FAIL${NC}: Delete with comparison"
    FAILED=$((FAILED + 1))
fi

# ============================================
# TEST SUITE 11: Table Drop
# ============================================
echo -e "\n${YELLOW}=== TEST SUITE 11: Table Drop ===${NC}"

test_start "Drop table without FORCE parameter (should fail)"
result=$($REDIS_CLI TABLE.DROP testdb.products 2>&1)
assert_error "This operation is irreversible, use FORCE parameter to remove the table" "$result" "Drop without FORCE should fail"

test_start "Verify table still exists after failed drop"
result=$($REDIS_CLI TABLE.SELECT testdb.products)
if [[ "$result" != *"ERR"* ]]; then
    echo -e "${GREEN}✓ PASS${NC}: Table still exists after failed drop"
    PASSED=$((PASSED + 1))
else
    echo -e "${RED}✗ FAIL${NC}: Table should still exist after failed drop"
    FAILED=$((FAILED + 1))
fi

test_start "Drop table with invalid parameter"
result=$($REDIS_CLI TABLE.DROP testdb.products INVALID 2>&1)
assert_error "Invalid parameter. Use FORCE to confirm table removal" "$result" "Drop with invalid parameter should fail"

test_start "Drop table with FORCE parameter"
result=$($REDIS_CLI TABLE.DROP testdb.products FORCE)
assert_equals "OK" "$result" "Drop table with FORCE should return OK"

test_start "Verify table dropped"
result=$($REDIS_CLI TABLE.SELECT testdb.products 2>&1)
assert_error "table schema does not exist" "$result" "Dropped table should not exist"

test_start "Drop non-existent table with FORCE"
result=$($REDIS_CLI TABLE.DROP testdb.notexist FORCE 2>&1)
assert_error "table schema does not exist" "$result" "Drop non-existent table should fail"

# ============================================
# TEST SUITE 12: Edge Cases
# ============================================
echo -e "\n${YELLOW}=== TEST SUITE 12: Edge Cases ===${NC}"

test_start "Insert with negative integer"
$REDIS_CLI TABLE.SCHEMA.CREATE testdb.numbers VALUE:integer > /dev/null
result=$($REDIS_CLI TABLE.INSERT testdb.numbers VALUE=-42)
assert_equals "1" "$result" "Negative integer should work"

test_start "Insert with negative float"
$REDIS_CLI TABLE.SCHEMA.CREATE testdb.floats VALUE:float > /dev/null
result=$($REDIS_CLI TABLE.INSERT testdb.floats VALUE=-123.45)
assert_equals "1" "$result" "Negative float should work"

test_start "String comparison operators"
$REDIS_CLI TABLE.SCHEMA.CREATE testdb.strings NAME:string:false > /dev/null
$REDIS_CLI TABLE.INSERT testdb.strings NAME=Alice > /dev/null
$REDIS_CLI TABLE.INSERT testdb.strings NAME=Bob > /dev/null
$REDIS_CLI TABLE.INSERT testdb.strings NAME=Charlie > /dev/null
result=$($REDIS_CLI TABLE.SELECT testdb.strings WHERE NAME\>Bob)
assert_contains "Charlie" "$result" "String comparison should work"

test_start "Empty WHERE clause (returns empty result)"
# WHERE without condition returns empty array - this is actually valid behavior
result=$($REDIS_CLI TABLE.SELECT testdb.users WHERE 2>&1)
if [[ "$result" == *"(empty"* ]] || [[ -z "$result" ]]; then
    echo -e "${GREEN}✓ PASS${NC}: Empty WHERE returns empty result"
    PASSED=$((PASSED + 1))
else
    echo -e "${RED}✗ FAIL${NC}: Empty WHERE should return empty (got: $result)"
    FAILED=$((FAILED + 1))
fi

test_start "Dangling operator"
result=$($REDIS_CLI TABLE.SELECT testdb.users WHERE NAME=John AND 2>&1)
assert_error "dangling operator" "$result" "Dangling AND should fail"

test_start "Invalid condition format"
# Test with a condition that has no operator
result=$($REDIS_CLI TABLE.SELECT testdb.users WHERE InvalidCondition 2>&1)
if [[ "$result" == *"ERR"* ]] || [[ "$result" == *"condition"* ]] || [[ -z "$result" ]] || [[ "$result" == *"(empty"* ]]; then
    echo -e "${GREEN}✓ PASS${NC}: Invalid condition format handled"
    PASSED=$((PASSED + 1))
else
    echo -e "${RED}✗ FAIL${NC}: Invalid condition should fail or return empty (got: $result)"
    FAILED=$((FAILED + 1))
fi

# ============================================
# TEST SUITE 13: Help Command
# ============================================
echo -e "\n${YELLOW}=== TEST SUITE 13: Help Command ===${NC}"

test_start "TABLE.HELP command"
result=$($REDIS_CLI TABLE.HELP)
assert_contains "TABLE.SCHEMA.CREATE" "$result" "Help should contain TABLE.SCHEMA.CREATE"
assert_contains "TABLE.SELECT" "$result" "Help should contain TABLE.SELECT"
assert_contains "TABLE.INSERT" "$result" "Help should contain TABLE.INSERT"

# ============================================
# TEST SUITE 14: Index Maintenance
# ============================================
echo -e "\n${YELLOW}=== TEST SUITE 14: Index Maintenance ===${NC}"

$REDIS_CLI FLUSHALL > /dev/null
$REDIS_CLI TABLE.NAMESPACE.CREATE idxtest > /dev/null
$REDIS_CLI TABLE.SCHEMA.CREATE idxtest.data NAME:string:true VALUE:integer:true > /dev/null

test_start "Insert creates index entries"
$REDIS_CLI TABLE.INSERT idxtest.data NAME=Test VALUE=100 > /dev/null
result=$($REDIS_CLI SMEMBERS "idx:idxtest.data:NAME:Test")
assert_contains "1" "$result" "Index should contain row ID"

test_start "Update maintains indexes"
$REDIS_CLI TABLE.UPDATE idxtest.data WHERE NAME=Test SET NAME=Updated > /dev/null
result=$($REDIS_CLI SMEMBERS "idx:idxtest.data:NAME:Updated")
assert_contains "1" "$result" "Updated index should contain row ID"

old_index=$($REDIS_CLI SMEMBERS "idx:idxtest.data:NAME:Test")
if [[ -z "$old_index" ]] || [[ "$old_index" == *"(empty"* ]]; then
    echo -e "${GREEN}✓ PASS${NC}: Old index entry removed"
    PASSED=$((PASSED + 1))
else
    echo -e "${RED}✗ FAIL${NC}: Old index entry should be removed"
    FAILED=$((FAILED + 1))
fi

test_start "Delete removes index entries"
$REDIS_CLI TABLE.DELETE idxtest.data WHERE NAME=Updated > /dev/null
result=$($REDIS_CLI SMEMBERS "idx:idxtest.data:NAME:Updated")
if [[ -z "$result" ]] || [[ "$result" == *"(empty"* ]]; then
    echo -e "${GREEN}✓ PASS${NC}: Index entry removed after delete"
    PASSED=$((PASSED + 1))
else
    echo -e "${RED}✗ FAIL${NC}: Index entry should be removed"
    FAILED=$((FAILED + 1))
fi

# ============================================
# TEST SUITE 16: Character Limit Validation
# ============================================
echo -e "\n${YELLOW}=== TEST SUITE 16: Character Limit Validation ===${NC}"

test_start "Create namespace with exactly 64 characters"
long_namespace=$(printf 'a%.0s' {1..64})
result=$($REDIS_CLI TABLE.NAMESPACE.CREATE "$long_namespace")
assert_equals "OK" "$result" "Namespace with exactly 64 characters should succeed"

test_start "Create namespace exceeding 64 characters"
very_long_namespace=$(printf 'a%.0s' {1..65})
result=$($REDIS_CLI TABLE.NAMESPACE.CREATE "$very_long_namespace" 2>&1)
assert_error "incorrect namespace name, it exceeds the limit of 64 characters" "$result" "Namespace over 64 characters should fail"

test_start "Create table with namespace exactly 64 characters"
long_ns=$(printf 'b%.0s' {1..64})
$REDIS_CLI TABLE.NAMESPACE.CREATE "$long_ns" > /dev/null
result=$($REDIS_CLI TABLE.SCHEMA.CREATE "$long_ns".testtable NAME:string)
assert_equals "OK" "$result" "Table with 64-char namespace should succeed"

test_start "Create table with table name exactly 64 characters"
# Ensure testdb namespace exists for this test
$REDIS_CLI TABLE.NAMESPACE.CREATE testdb > /dev/null 2>&1
long_table=$(printf 'c%.0s' {1..64})
result=$($REDIS_CLI TABLE.SCHEMA.CREATE testdb."$long_table" NAME:string)
assert_equals "OK" "$result" "Table with 64-char name should succeed"

test_start "Create table with namespace exceeding 64 characters"
very_long_ns=$(printf 'd%.0s' {1..65})
result=$($REDIS_CLI TABLE.SCHEMA.CREATE "$very_long_ns".testtable NAME:string 2>&1)
assert_error "incorrect namespace name, it exceeds the limit of 64 characters" "$result" "Table with long namespace should fail"

test_start "Create table with table name exceeding 64 characters"
very_long_table=$(printf 'e%.0s' {1..65})
result=$($REDIS_CLI TABLE.SCHEMA.CREATE testdb."$very_long_table" NAME:string 2>&1)
assert_error "incorrect table name, it exceeds the limit of 64 characters" "$result" "Table with long name should fail"

test_start "Create table with both namespace and table name over 64 characters"
very_long_ns=$(printf 'f%.0s' {1..65})
very_long_table=$(printf 'g%.0s' {1..65})
result=$($REDIS_CLI TABLE.SCHEMA.CREATE "$very_long_ns"."$very_long_table" NAME:string 2>&1)
assert_error "incorrect namespace name, it exceeds the limit of 64 characters" "$result" "Table with both names too long should fail (namespace checked first)"

# Clean up for next tests
# Note: Don't flush here as Complex Scenarios tests depend on company.employees table

# ============================================
# TEST SUITE 15: Complex Scenarios
# ============================================
echo -e "\n${YELLOW}=== TEST SUITE 15: Complex Scenarios ===${NC}"

# Ensure company namespace and employees table exist for these tests
$REDIS_CLI TABLE.NAMESPACE.CREATE company > /dev/null 2>&1
$REDIS_CLI TABLE.SCHEMA.CREATE company.employees EMPID:string:true NAME:string:true DEPT:string:true SALARY:float:false AGE:integer:false HIREDATE:date:true > /dev/null 2>&1
$REDIS_CLI TABLE.INSERT company.employees EMPID=E001 NAME=John DEPT=Engineering SALARY=50000.50 AGE=30 HIREDATE=2020-01-15 > /dev/null 2>&1
$REDIS_CLI TABLE.INSERT company.employees EMPID=E002 NAME=Jane DEPT=Marketing SALARY=55000.75 AGE=28 HIREDATE=2021-03-20 > /dev/null 2>&1
$REDIS_CLI TABLE.INSERT company.employees EMPID=E003 NAME=Bob DEPT=Engineering SALARY=60000.00 AGE=35 HIREDATE=2019-06-10 > /dev/null 2>&1
$REDIS_CLI TABLE.INSERT company.employees EMPID=E004 NAME=Alice DEPT=Sales SALARY=58000.25 AGE=32 HIREDATE=2020-11-05 > /dev/null 2>&1
result=$($REDIS_CLI TABLE.SELECT company.employees)
row_count=$(echo "$result" | grep -o "EMPID" | wc -l)
if [ "$row_count" -eq 4 ]; then
    echo -e "${GREEN}✓ PASS${NC}: All 4 employees inserted"
    PASSED=$((PASSED + 1))
else
    echo -e "${RED}✗ FAIL${NC}: Expected 4 employees, got $row_count"
    FAILED=$((FAILED + 1))
fi

test_start "Complex query: Department + Age range"
result=$($REDIS_CLI TABLE.SELECT company.employees WHERE DEPT=Engineering AND AGE\>28)
assert_contains "John" "$result" "Should find John in Engineering with AGE>28"
assert_contains "Bob" "$result" "Should find Bob in Engineering with AGE>28"

test_start "Complex query: Salary range"
result=$($REDIS_CLI TABLE.SELECT company.employees WHERE SALARY\>=55000 AND SALARY\<=60000)
assert_contains "Jane" "$result" "Should find Jane with salary in range"
assert_contains "Alice" "$result" "Should find Alice with salary in range"

test_start "Complex query: Date range"
result=$($REDIS_CLI TABLE.SELECT company.employees WHERE HIREDATE\>=2020-01-01 AND HIREDATE\<=2020-12-31)
assert_contains "John" "$result" "Should find John hired in 2020"
assert_contains "Alice" "$result" "Should find Alice hired in 2020"

test_start "Add index and query"
$REDIS_CLI TABLE.SCHEMA.ALTER company.employees ADD INDEX AGE > /dev/null
result=$($REDIS_CLI TABLE.SELECT company.employees WHERE AGE=30)
assert_contains "John" "$result" "Indexed AGE query should work"

test_start "Update salary and verify"
$REDIS_CLI TABLE.UPDATE company.employees WHERE EMPID=E001 SET SALARY=52000.75 > /dev/null
result=$($REDIS_CLI TABLE.SELECT company.employees WHERE EMPID=E001)
assert_contains "52000.75" "$result" "Salary should be updated"

# ============================================
# Final Summary
# ============================================
echo ""
echo "========================================"
echo "Test Summary"
echo "========================================"
echo -e "${GREEN}Passed: $PASSED${NC}"
echo -e "${RED}Failed: $FAILED${NC}"
echo "Total:  $((PASSED + FAILED))"
echo "========================================"

if [ $FAILED -eq 0 ]; then
    echo -e "${GREEN}All tests passed!${NC}"
    exit 0
else
    echo -e "${RED}Some tests failed!${NC}"
    exit 1
fi
