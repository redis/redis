SRC_DIR = src
MODULE_VERSION = v8.2.1
MODULE_REPO = https://github.com/redisjson/redisjson
TARGET_MODULE = $(SRC_DIR)/bin/$(FULL_VARIANT)/rejson.so

include ../common.mk

$(SRC_DIR)/.cargo_fetched:
	cd $(SRC_DIR) && cargo fetch

get_source: $(SRC_DIR)/.cargo_fetched
