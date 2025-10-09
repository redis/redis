# Redis Table Module Makefile
# Author: Raphael Drai
# Date: October 3, 2025

# Configuration
MODULE_NAME = redis_table
REDIS_SRC = ../../src
CC = gcc
CFLAGS = -Wall -Werror -g -O0 -fPIC -I$(REDIS_SRC)

# Default target: build the module
all: $(MODULE_NAME).so

# Build the shared library
$(MODULE_NAME).so: $(MODULE_NAME).o
	$(CC) -shared -o $@ $^

# Compile the source file
$(MODULE_NAME).o: $(MODULE_NAME).c
	$(CC) $(CFLAGS) -c $<

# Clean build artifacts
clean:
	rm -f $(MODULE_NAME).o $(MODULE_NAME).so

# Run tests
test: $(MODULE_NAME).so
	@echo "Running Redis Table Module tests..."
	@cd tests && ./run_tests.sh

# Run client compatibility tests (optional - skips if dependencies missing)
test-clients: $(MODULE_NAME).so
	@echo "Running client compatibility tests..."
	@echo "Note: Tests will be skipped if dependencies are not installed"
	@cd tests && ./run_client_tests.sh

# Run all tests (unit tests + client tests)
test-all: test test-clients

# Run memory leak tests
test-memory: $(MODULE_NAME).so
	@echo "Running memory leak detection tests..."
	@cd tests && ./test_memory_leaks.sh

# Run memory profiler (requires python3-redis)
# Note: May report warnings for expected memory growth patterns
test-memory-profiler: $(MODULE_NAME).so
	@echo "Running memory profiler..."
	@cd tests && python3 test_memory_profiler.py || true

# Debug target - build and provide debug info
debug: $(MODULE_NAME).so
	@echo "Module built: $(MODULE_NAME).so"
	@echo "To debug: gdb --args $(REDIS_SRC)/redis-server --loadmodule $(PWD)/$(MODULE_NAME).so"

# Install target (optional)
install: $(MODULE_NAME).so
	@echo "Installing $(MODULE_NAME).so to /usr/local/lib/redis/modules/"
	@mkdir -p /usr/local/lib/redis/modules/
	@cp $(MODULE_NAME).so /usr/local/lib/redis/modules/

.PHONY: all clean test test-clients test-all test-memory test-memory-profiler debug install
