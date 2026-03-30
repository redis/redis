SRC_DIR = src
TARGET_MODULE = $(SRC_DIR)/bin/$(FULL_VARIANT)/rejson.so

include ../common.mk

get_source: cargo_fetch

cargo_fetch:
	@if [ -d "$(SRC_DIR)" ] && command -v cargo >/dev/null 2>&1; then \
		if [ ! -f .cargo_fetched ] || [ "$(SRC_DIR)/Cargo.lock" -nt .cargo_fetched ]; then \
			echo "Fetching Rust crate dependencies (cargo fetch)"; \
			(cd "$(SRC_DIR)" && cargo fetch); \
			touch .cargo_fetched; \
		fi; \
	fi

.PHONY: cargo_fetch
