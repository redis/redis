#!/bin/bash
#
# Redis Table Module - Client Compatibility Test Runner
# Runs tests for multiple programming languages to verify special character handling
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REDIS_DIR="$SCRIPT_DIR/../../.."
MODULE_PATH="$SCRIPT_DIR/../redis_table.so"

echo "========================================"
echo "Redis Table Module - Client Tests"
echo "========================================"
echo

# Check if module exists
if [ ! -f "$MODULE_PATH" ]; then
    echo "ERROR: Module not found at $MODULE_PATH"
    echo "Run 'make' first to build the module"
    exit 1
fi

# Start Redis with module
echo "Starting Redis server with table module..."
cd "$REDIS_DIR"

# Kill any existing Redis
pkill -9 redis-server 2>/dev/null || true
sleep 1

# Start Redis
./src/redis-server --loadmodule "$MODULE_PATH" --daemonize yes --port 6379
sleep 2

# Verify Redis is running
if ! ./src/redis-cli ping > /dev/null 2>&1; then
    echo "ERROR: Redis server failed to start"
    exit 1
fi

echo "✓ Redis server started"
echo

# Track results
TOTAL_TESTS=0
PASSED_TESTS=0
FAILED_TESTS=0

# Test Python client
echo "========================================"
echo "Test 1: Python (redis-py) Client"
echo "========================================"
TOTAL_TESTS=$((TOTAL_TESTS + 1))

if command -v python3 &> /dev/null; then
    if python3 -c "import redis" 2>/dev/null; then
        if cd "$SCRIPT_DIR" && python3 test_client_compatibility.py; then
            echo "✓ Python client tests PASSED"
            PASSED_TESTS=$((PASSED_TESTS + 1))
        else
            echo "✗ Python client tests FAILED"
            FAILED_TESTS=$((FAILED_TESTS + 1))
        fi
    else
        echo "⚠ SKIPPED: redis-py not installed (pip install redis)"
        TOTAL_TESTS=$((TOTAL_TESTS - 1))
    fi
else
    echo "⚠ SKIPPED: Python 3 not installed"
    TOTAL_TESTS=$((TOTAL_TESTS - 1))
fi

echo

# Test Node.js client
echo "========================================"
echo "Test 2: Node.js (node-redis) Client"
echo "========================================"
TOTAL_TESTS=$((TOTAL_TESTS + 1))

if command -v node &> /dev/null; then
    if node -e "require('redis')" 2>/dev/null; then
        if cd "$SCRIPT_DIR" && node test_client_compatibility.js; then
            echo "✓ Node.js client tests PASSED"
            PASSED_TESTS=$((PASSED_TESTS + 1))
        else
            echo "✗ Node.js client tests FAILED"
            FAILED_TESTS=$((FAILED_TESTS + 1))
        fi
    else
        echo "⚠ SKIPPED: node-redis not installed (npm install redis)"
        TOTAL_TESTS=$((TOTAL_TESTS - 1))
    fi
else
    echo "⚠ SKIPPED: Node.js not installed"
    TOTAL_TESTS=$((TOTAL_TESTS - 1))
fi

echo

# Stop Redis
echo "Stopping Redis server..."
cd "$REDIS_DIR"
./src/redis-cli shutdown nosave 2>/dev/null || true
sleep 1

echo "Redis server stopped"
echo

# Summary
echo "========================================"
echo "Client Compatibility Test Summary"
echo "========================================"
echo "Total Tests Run: $TOTAL_TESTS"
echo "Passed:          $PASSED_TESTS"
echo "Failed:          $FAILED_TESTS"
echo "========================================"

if [ $FAILED_TESTS -eq 0 ] && [ $TOTAL_TESTS -gt 0 ]; then
    echo "✓ All client compatibility tests passed!"
    exit 0
elif [ $TOTAL_TESTS -eq 0 ]; then
    echo "⚠ No client tests could run (missing dependencies)"
    echo ""
    echo "To install dependencies:"
    echo ""
    echo "  Python option 1 (system package):"
    echo "    sudo apt install python3-redis"
    echo ""
    echo "  Python option 2 (virtual environment):"
    echo "    python3 -m venv venv"
    echo "    source venv/bin/activate"
    echo "    pip install redis"
    echo "    # Then run: ./tests/test_client_compatibility.py"
    echo ""
    echo "  Node.js:"
    echo "    Install from https://nodejs.org/"
    echo "    npm install redis"
    echo ""
    echo "ℹ️  Client tests are optional. Run 'make test' for core module tests."
    exit 0  # Exit with success - missing dependencies is not a test failure
else
    echo "✗ Some client tests failed"
    exit 1
fi
