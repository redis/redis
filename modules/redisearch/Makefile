SRC_DIR = src
MODULE_VERSION = v8.8.0
MODULE_REPO = https://github.com/redisearch/redisearch
TARGET_MODULE = $(SRC_DIR)/bin/$(FULL_VARIANT)/search-community/redisearch.so

# Enable link-time optimization for RediSearch by default. Override with LTO=0.
LTO ?= 1
export LTO

# Use the committed C headers for Rust modules, rather than regenerating them
# from Rust source. Override with REDISEARCH_GENERATE_HEADERS=1.
REDISEARCH_GENERATE_HEADERS ?= 0
export REDISEARCH_GENERATE_HEADERS

  # Set INLINE_LSE_ATOMICS=1 for perf improvement on common ARM CPUs (i.e. Graviton2/3/4); no effect on x86 or macOS.
  # Default 0 keeps the binary runnable on pre-Armv8.1-a cores (Cortex-A72, Graviton1, RPi4) that would otherwise SIGILL at module load.
INLINE_LSE_ATOMICS ?= 0
export INLINE_LSE_ATOMICS

include ../common.mk
