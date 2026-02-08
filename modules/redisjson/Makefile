SRC_DIR = src
MODULE_VERSION = v8.5.90
MODULE_REPO = https://github.com/redisjson/redisjson
TARGET_MODULE = $(SRC_DIR)/bin/$(FULL_VARIANT)/rejson.so

include ../common.mk

CARGO_FETCHED_MARKER := .cargo_fetched

get_source_post:
	@if [ ! -f "$(CARGO_FETCHED_MARKER)" ] || [ "$(SRC_DIR)/Cargo.lock" -nt "$(CARGO_FETCHED_MARKER)" ]; then \
		echo "Fetching Rust crate dependencies (cargo fetch)"; \
		( cd "$(SRC_DIR)" && cargo fetch ); \
		touch "$(CARGO_FETCHED_MARKER)"; \
	fi

pristine_post:
	-rm -f "$(CARGO_FETCHED_MARKER)"
