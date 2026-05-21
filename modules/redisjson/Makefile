SRC_DIR = src
MODULE_VERSION = v8.7.91
MODULE_REPO = https://github.com/redisjson/redisjson
TARGET_MODULE = $(SRC_DIR)/bin/$(FULL_VARIANT)/rejson.so

include ../common.mk

$(SRC_DIR)/.cargo_fetched: $(SRC_DIR)/.prepared
	cd $(SRC_DIR) && cargo fetch
	touch $@

get_source: $(SRC_DIR)/.cargo_fetched
