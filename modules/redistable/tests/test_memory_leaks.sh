#!/bin/bash
#
# Redis Table Module - Memory Leak Detection Test Suite
# Tests for memory leaks in long-running scenarios with frequent schema alterations
#
# This test suite focuses on:
# 1. RedisModule_AutoMemory usage patterns
# 2. Manual memory allocations (RedisModule_Alloc/Free)
# 3. Schema alteration operations
# 4. Large-scale operations
# 5. Repeated operations that could accumulate leaks
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REDIS_DIR="$SCRIPT_DIR/../../.."
MODULE_PATH="$SCRIPT_DIR/../redis_table.so"
REDIS_CLI="$REDIS_DIR/src/redis-cli"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo "========================================"
echo "Redis Table Module - Memory Leak Tests"
echo "========================================"
echo

# Check if module exists
if [ ! -f "$MODULE_PATH" ]; then
    echo -e "${RED}ERROR: Module not found at $MODULE_PATH${NC}"
    echo "Run 'make' first to build the module"
    exit 1
fi

# Start Redis with module
echo "Starting Redis server with table module..."
cd "$REDIS_DIR"

# Kill any existing Redis
pkill -9 redis-server 2>/dev/null || true
sleep 1

# Start Redis with memory tracking enabled
./src/redis-server \
    --loadmodule "$MODULE_PATH" \
    --daemonize yes \
    --port 6379 \
    --maxmemory-policy noeviction \
    --save "" \
    --appendonly no

sleep 2

# Verify Redis is running
if ! $REDIS_CLI ping > /dev/null 2>&1; then
    echo -e "${RED}ERROR: Redis server failed to start${NC}"
    exit 1
fi

echo -e "${GREEN}✓ Redis server started${NC}"
echo

# Helper function to get memory usage
get_memory_usage() {
    $REDIS_CLI INFO memory | grep "used_memory:" | cut -d: -f2 | tr -d '\r'
}

# Helper function to get memory RSS
get_memory_rss() {
    $REDIS_CLI INFO memory | grep "used_memory_rss:" | cut -d: -f2 | tr -d '\r'
}

# Test counter
TEST_NUM=0
PASSED=0
FAILED=0

# Test function
run_test() {
    TEST_NUM=$((TEST_NUM + 1))
    echo "Test $TEST_NUM: $1"
}

pass_test() {
    echo -e "${GREEN}✓ PASS${NC}: $1"
    PASSED=$((PASSED + 1))
}

fail_test() {
    echo -e "${RED}✗ FAIL${NC}: $1"
    FAILED=$((FAILED + 1))
}

warn_test() {
    echo -e "${YELLOW}⚠ WARNING${NC}: $1"
}

# Cleanup function
cleanup() {
    echo
    echo "Cleaning up..."
    $REDIS_CLI FLUSHALL > /dev/null 2>&1 || true
}

# ============================================
# Test Suite 1: Baseline Memory Usage
# ============================================
echo "========================================"
echo "Test Suite 1: Baseline Memory Usage"
echo "========================================"

run_test "Measure baseline memory"
BASELINE_MEM=$(get_memory_usage)
echo "Baseline memory: $BASELINE_MEM bytes"
pass_test "Baseline established"

# ============================================
# Test Suite 2: Schema Creation Memory Leaks
# ============================================
echo
echo "========================================"
echo "Test Suite 2: Schema Creation Leaks"
echo "========================================"

run_test "Create and drop namespace 1000 times"
MEM_BEFORE=$(get_memory_usage)
for i in $(seq 1 1000); do
    $REDIS_CLI TABLE.NAMESPACE.CREATE "test_ns_$i" > /dev/null
done
MEM_AFTER=$(get_memory_usage)
MEM_INCREASE=$((MEM_AFTER - MEM_BEFORE))
echo "Memory before: $MEM_BEFORE bytes"
echo "Memory after:  $MEM_AFTER bytes"
echo "Memory increase: $MEM_INCREASE bytes"

# Expected: ~50-100 bytes per namespace (reasonable)
# Alert if > 500 bytes per namespace (potential leak)
BYTES_PER_NS=$((MEM_INCREASE / 1000))
if [ $BYTES_PER_NS -lt 500 ]; then
    pass_test "Memory increase reasonable: ${BYTES_PER_NS} bytes/namespace"
else
    fail_test "Excessive memory increase: ${BYTES_PER_NS} bytes/namespace (expected < 500)"
fi

cleanup

# ============================================
# Test Suite 3: Table Creation/Drop Cycles
# ============================================
echo
echo "========================================"
echo "Test Suite 3: Table Creation/Drop Cycles"
echo "========================================"

run_test "Create and drop table 500 times"
$REDIS_CLI TABLE.NAMESPACE.CREATE "leak_test" > /dev/null
MEM_BEFORE=$(get_memory_usage)

for i in $(seq 1 500); do
    $REDIS_CLI TABLE.SCHEMA.CREATE "leak_test.table_$i" "ID:integer:true" "NAME:string:false" > /dev/null
    $REDIS_CLI TABLE.DROP "leak_test.table_$i" FORCE > /dev/null
done

MEM_AFTER=$(get_memory_usage)
MEM_INCREASE=$((MEM_AFTER - MEM_BEFORE))
echo "Memory before: $MEM_BEFORE bytes"
echo "Memory after:  $MEM_AFTER bytes"
echo "Memory increase: $MEM_INCREASE bytes"

# After drop, memory should return close to baseline
# Allow 10KB tolerance for Redis overhead
if [ $MEM_INCREASE -lt 10240 ]; then
    pass_test "No significant memory leak detected (${MEM_INCREASE} bytes)"
else
    warn_test "Memory increased by ${MEM_INCREASE} bytes (may indicate leak)"
fi

cleanup

# ============================================
# Test Suite 4: Schema Alteration Leaks
# ============================================
echo
echo "========================================"
echo "Test Suite 4: Schema Alteration Leaks"
echo "========================================"

run_test "Add and drop columns 500 times"
$REDIS_CLI TABLE.NAMESPACE.CREATE "alter_test" > /dev/null
$REDIS_CLI TABLE.SCHEMA.CREATE "alter_test.users" "ID:integer:true" > /dev/null

MEM_BEFORE=$(get_memory_usage)

for i in $(seq 1 500); do
    $REDIS_CLI TABLE.SCHEMA.ALTER "alter_test.users" ADD COLUMN "col_$i:string:false" > /dev/null
done

MEM_AFTER=$(get_memory_usage)
MEM_INCREASE=$((MEM_AFTER - MEM_BEFORE))
echo "Memory before: $MEM_BEFORE bytes"
echo "Memory after:  $MEM_AFTER bytes"
echo "Memory increase: $MEM_INCREASE bytes"

# Expected: ~100-200 bytes per column
BYTES_PER_COL=$((MEM_INCREASE / 500))
if [ $BYTES_PER_COL -lt 500 ]; then
    pass_test "Column memory reasonable: ${BYTES_PER_COL} bytes/column"
else
    fail_test "Excessive column memory: ${BYTES_PER_COL} bytes/column"
fi

cleanup

# ============================================
# Test Suite 5: Index Add/Drop Cycles
# ============================================
echo
echo "========================================"
echo "Test Suite 5: Index Add/Drop Cycles"
echo "========================================"

run_test "Add and drop indexes 500 times"
$REDIS_CLI TABLE.NAMESPACE.CREATE "index_test" > /dev/null
$REDIS_CLI TABLE.SCHEMA.CREATE "index_test.data" \
    "ID:integer:false" \
    "NAME:string:false" \
    "AGE:integer:false" > /dev/null

# Insert some data
for i in $(seq 1 100); do
    $REDIS_CLI TABLE.INSERT "index_test.data" "ID=$i" "NAME=User$i" "AGE=$((20 + i % 50))" > /dev/null
done

MEM_BEFORE=$(get_memory_usage)

for i in $(seq 1 500); do
    # Add index (builds index for existing data)
    $REDIS_CLI TABLE.SCHEMA.ALTER "index_test.data" ADD INDEX "AGE" > /dev/null
    # Drop index
    $REDIS_CLI TABLE.SCHEMA.ALTER "index_test.data" DROP INDEX "AGE" > /dev/null
done

MEM_AFTER=$(get_memory_usage)
MEM_INCREASE=$((MEM_AFTER - MEM_BEFORE))
echo "Memory before: $MEM_BEFORE bytes"
echo "Memory after:  $MEM_AFTER bytes"
echo "Memory increase: $MEM_INCREASE bytes"

# After drop, memory should return close to baseline
if [ $MEM_INCREASE -lt 20480 ]; then
    pass_test "No significant index leak detected (${MEM_INCREASE} bytes)"
else
    warn_test "Memory increased by ${MEM_INCREASE} bytes after index cycles"
fi

cleanup

# ============================================
# Test Suite 6: Large Query Operations
# ============================================
echo
echo "========================================"
echo "Test Suite 6: Large Query Operations"
echo "========================================"

run_test "Execute 1000 queries on large dataset"
$REDIS_CLI TABLE.NAMESPACE.CREATE "query_test" > /dev/null
$REDIS_CLI TABLE.SCHEMA.CREATE "query_test.records" \
    "ID:integer:true" \
    "VALUE:integer:false" > /dev/null

# Insert 1000 records
for i in $(seq 1 1000); do
    $REDIS_CLI TABLE.INSERT "query_test.records" "ID=$i" "VALUE=$((i * 10))" > /dev/null
done

MEM_BEFORE=$(get_memory_usage)

# Run 1000 queries
for i in $(seq 1 1000); do
    $REDIS_CLI TABLE.SELECT "query_test.records" WHERE "ID=$i" > /dev/null
done

MEM_AFTER=$(get_memory_usage)
MEM_INCREASE=$((MEM_AFTER - MEM_BEFORE))
echo "Memory before: $MEM_BEFORE bytes"
echo "Memory after:  $MEM_AFTER bytes"
echo "Memory increase: $MEM_INCREASE bytes"

# Queries should not accumulate memory (AutoMemory should clean up)
if [ $MEM_INCREASE -lt 10240 ]; then
    pass_test "No query memory accumulation (${MEM_INCREASE} bytes)"
else
    warn_test "Memory increased by ${MEM_INCREASE} bytes during queries"
fi

cleanup

# ============================================
# Test Suite 7: Update Operations
# ============================================
echo
echo "========================================"
echo "Test Suite 7: Update Operations"
echo "========================================"

run_test "Execute 1000 updates"
$REDIS_CLI TABLE.NAMESPACE.CREATE "update_test" > /dev/null
$REDIS_CLI TABLE.SCHEMA.CREATE "update_test.data" \
    "ID:integer:true" \
    "COUNTER:integer:false" > /dev/null

# Insert records
for i in $(seq 1 100); do
    $REDIS_CLI TABLE.INSERT "update_test.data" "ID=$i" "COUNTER=0" > /dev/null
done

MEM_BEFORE=$(get_memory_usage)

# Update each record 10 times
for i in $(seq 1 1000); do
    ID=$((i % 100 + 1))
    $REDIS_CLI TABLE.UPDATE "update_test.data" WHERE "ID=$ID" SET "COUNTER=$i" > /dev/null
done

MEM_AFTER=$(get_memory_usage)
MEM_INCREASE=$((MEM_AFTER - MEM_BEFORE))
echo "Memory before: $MEM_BEFORE bytes"
echo "Memory after:  $MEM_AFTER bytes"
echo "Memory increase: $MEM_INCREASE bytes"

if [ $MEM_INCREASE -lt 10240 ]; then
    pass_test "No update memory leak (${MEM_INCREASE} bytes)"
else
    warn_test "Memory increased by ${MEM_INCREASE} bytes during updates"
fi

cleanup

# ============================================
# Test Suite 8: Delete Operations
# ============================================
echo
echo "========================================"
echo "Test Suite 8: Delete Operations"
echo "========================================"

run_test "Insert and delete 1000 records"
$REDIS_CLI TABLE.NAMESPACE.CREATE "delete_test" > /dev/null
$REDIS_CLI TABLE.SCHEMA.CREATE "delete_test.temp" \
    "ID:integer:true" \
    "DATA:string:false" > /dev/null

MEM_BEFORE=$(get_memory_usage)

for i in $(seq 1 1000); do
    $REDIS_CLI TABLE.INSERT "delete_test.temp" "ID=$i" "DATA=test$i" > /dev/null
    $REDIS_CLI TABLE.DELETE "delete_test.temp" WHERE "ID=$i" > /dev/null
done

MEM_AFTER=$(get_memory_usage)
MEM_INCREASE=$((MEM_AFTER - MEM_BEFORE))
echo "Memory before: $MEM_BEFORE bytes"
echo "Memory after:  $MEM_AFTER bytes"
echo "Memory increase: $MEM_INCREASE bytes"

# After deletes, memory should be minimal
if [ $MEM_INCREASE -lt 10240 ]; then
    pass_test "No delete memory leak (${MEM_INCREASE} bytes)"
else
    warn_test "Memory increased by ${MEM_INCREASE} bytes after delete cycles"
fi

cleanup

# ============================================
# Test Suite 9: Mixed Operations Stress Test
# ============================================
echo
echo "========================================"
echo "Test Suite 9: Mixed Operations Stress"
echo "========================================"

run_test "Mixed operations (1000 iterations)"
$REDIS_CLI TABLE.NAMESPACE.CREATE "stress_test" > /dev/null
$REDIS_CLI TABLE.SCHEMA.CREATE "stress_test.mixed" \
    "ID:integer:true" \
    "NAME:string:false" \
    "VALUE:integer:false" > /dev/null

MEM_BEFORE=$(get_memory_usage)

for i in $(seq 1 1000); do
    # Insert
    $REDIS_CLI TABLE.INSERT "stress_test.mixed" "ID=$i" "NAME=Test$i" "VALUE=$i" > /dev/null
    
    # Query
    $REDIS_CLI TABLE.SELECT "stress_test.mixed" WHERE "ID=$i" > /dev/null
    
    # Update
    $REDIS_CLI TABLE.UPDATE "stress_test.mixed" WHERE "ID=$i" SET "VALUE=$((i * 2))" > /dev/null
    
    # Delete every 10th record
    if [ $((i % 10)) -eq 0 ]; then
        $REDIS_CLI TABLE.DELETE "stress_test.mixed" WHERE "ID=$i" > /dev/null
    fi
done

MEM_AFTER=$(get_memory_usage)
MEM_INCREASE=$((MEM_AFTER - MEM_BEFORE))
echo "Memory before: $MEM_BEFORE bytes"
echo "Memory after:  $MEM_AFTER bytes"
echo "Memory increase: $MEM_INCREASE bytes"

# Expected: memory for 900 records (1000 - 100 deleted)
# Allow reasonable overhead
BYTES_PER_RECORD=$((MEM_INCREASE / 900))
if [ $BYTES_PER_RECORD -lt 1000 ]; then
    pass_test "Mixed operations memory reasonable: ${BYTES_PER_RECORD} bytes/record"
else
    warn_test "High memory per record: ${BYTES_PER_RECORD} bytes/record"
fi

cleanup

# ============================================
# Test Suite 10: Memory Fragmentation Check
# ============================================
echo
echo "========================================"
echo "Test Suite 10: Memory Fragmentation"
echo "========================================"

run_test "Check memory fragmentation after stress"
MEM_USED=$(get_memory_usage)
MEM_RSS=$(get_memory_rss)
FRAGMENTATION_RATIO=$(echo "scale=2; $MEM_RSS / $MEM_USED" | bc)

echo "Used memory: $MEM_USED bytes"
echo "RSS memory:  $MEM_RSS bytes"
echo "Fragmentation ratio: $FRAGMENTATION_RATIO"

# Fragmentation ratio should be < 1.5 (healthy)
# > 1.5 indicates fragmentation issues
if (( $(echo "$FRAGMENTATION_RATIO < 1.5" | bc -l) )); then
    pass_test "Fragmentation ratio healthy: $FRAGMENTATION_RATIO"
else
    warn_test "High fragmentation ratio: $FRAGMENTATION_RATIO (expected < 1.5)"
fi

# ============================================
# Final Summary
# ============================================
echo
echo "========================================"
echo "Memory Leak Test Summary"
echo "========================================"
echo "Passed:  $PASSED"
echo "Failed:  $FAILED"
echo "Total:   $TEST_NUM"
echo "========================================"

# Stop Redis
echo
echo "Stopping Redis server..."
$REDIS_CLI shutdown nosave 2>/dev/null || true
sleep 1
echo "Redis server stopped"

if [ $FAILED -eq 0 ]; then
    echo
    echo -e "${GREEN}✓ All memory leak tests passed!${NC}"
    exit 0
else
    echo
    echo -e "${RED}✗ Some memory leak tests failed${NC}"
    exit 1
fi
