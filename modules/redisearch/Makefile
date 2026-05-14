SRC_DIR = src
MODULE_VERSION = v8.7.91
MODULE_REPO = https://github.com/redisearch/redisearch
TARGET_MODULE = $(SRC_DIR)/bin/$(FULL_VARIANT)/search-community/redisearch.so

# Enable link-time optimization for RediSearch by default. Override with LTO=0.
LTO ?= 1
export LTO

include ../common.mk

