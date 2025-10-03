#!/bin/bash

# Redis Table Module Testing
# Author: Raphael Drai
# Email: raphael.drai@gmail.com
# Date: October 3, 2025

# Redis Table Module - Test Runner Script
# Convenience script to start Redis, run tests, and clean up

REDIS_DIR="../../.."
MODULE_PATH="$(pwd)/../redis_table.so"
REDIS_PID_FILE="/tmp/redis_table_test.pid"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}========================================"
echo "Redis Table Module - Test Runner"
echo -e "========================================${NC}\n"

# Check if module exists
if [ ! -f "$MODULE_PATH" ]; then
    echo -e "${RED}Error: Module not found at $MODULE_PATH${NC}"
    echo "Please run 'make' first to build the module."
    exit 1
fi

# Check if Redis is already running
if pgrep -x redis-server > /dev/null; then
    echo -e "${YELLOW}Warning: Redis server is already running${NC}"
    echo "Stopping existing Redis instance..."
    $REDIS_DIR/src/redis-cli SHUTDOWN NOSAVE 2>/dev/null
    sleep 1
    pkill -9 redis-server 2>/dev/null
    sleep 1
fi

echo -e "${GREEN}Starting Redis server with table module...${NC}"
cd $REDIS_DIR
./src/redis-server --loadmodule $MODULE_PATH --daemonize yes --pidfile $REDIS_PID_FILE
sleep 1
cd - > /dev/null

# Verify Redis started
if ! pgrep -x redis-server > /dev/null; then
    echo -e "${RED}Error: Failed to start Redis server${NC}"
    exit 1
fi
echo -e "${GREEN}Redis server started successfully${NC}\n"

# Clean database before tests
echo -e "${YELLOW}Cleaning database...${NC}"
$REDIS_DIR/src/redis-cli FLUSHALL > /dev/null

# Run tests
echo -e "${GREEN}Running test suite...${NC}\n"
./test_redis_table.sh
TEST_EXIT_CODE=$?

# Cleanup - Always stop Redis after tests
echo -e "\n${YELLOW}Stopping Redis server...${NC}"
$REDIS_DIR/src/redis-cli SHUTDOWN NOSAVE 2>/dev/null
sleep 1

# Force kill if still running
if [ -f "$REDIS_PID_FILE" ]; then
    PID=$(cat $REDIS_PID_FILE)
    if ps -p $PID > /dev/null 2>&1; then
        kill -9 $PID 2>/dev/null
    fi
    rm -f $REDIS_PID_FILE
fi
echo -e "${GREEN}Redis server stopped${NC}"

echo ""
if [ $TEST_EXIT_CODE -eq 0 ]; then
    echo -e "${GREEN}========================================${NC}"
    echo -e "${GREEN}All tests completed successfully!${NC}"
    echo -e "${GREEN}========================================${NC}"
else
    echo -e "${RED}========================================${NC}"
    echo -e "${RED}Some tests failed!${NC}"
    echo -e "${RED}========================================${NC}"
fi

exit $TEST_EXIT_CODE
