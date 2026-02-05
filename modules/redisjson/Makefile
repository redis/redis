SRC_DIR = src
MODULE_VERSION = v8.5.90
MODULE_REPO = https://github.com/redisjson/redisjson
TARGET_MODULE = $(SRC_DIR)/bin/$(FULL_VARIANT)/rejson.so

include ../common.mk

CARGO_FETCHED_MARKER := .cargo_fetched

get_source: $(CARGO_FETCHED_MARKER)

$(CARGO_FETCHED_MARKER): get_source_base
	cd $(SRC_DIR) && cargo fetch
	touch $@
