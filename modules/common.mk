PREFIX ?= /usr/local
INSTALL_DIR ?= $(DESTDIR)$(PREFIX)/lib/redis/modules
INSTALL ?= install

# This logic expects each module's source to exist under $(SRC_DIR).
# In this repo, module sources are tracked as **git submodules** at:
#   modules/<module_name>/$(SRC_DIR)
#
# Building will auto-init/update the submodule if it's missing, so:
# - `git clone --recurse-submodules ...` works out of the box
# - existing checkouts don't re-download anything
# - non-recursive clones still "just work" on first build

ARCH_MAP_x86_64 := x64
ARCH_MAP_i386 := x86
ARCH_MAP_i686 := x86
ARCH_MAP_aarch64 := arm64v8
ARCH_MAP_arm64 := arm64v8

OS := $(shell uname -s | tr '[:upper:]' '[:lower:]')
ARCH := $(ARCH_MAP_$(shell uname -m))
ifeq ($(ARCH),)
	$(error Unrecognized CPU architecture $(shell uname -m))
endif

FULL_VARIANT := $(OS)-$(ARCH)-release

# Common rules for all modules, based on per-module configuration

all: $(TARGET_MODULE)

$(TARGET_MODULE): get_source
	$(MAKE) -C $(SRC_DIR)
	cp ${TARGET_MODULE} ./

ROOT_DIR := $(abspath $(CURDIR)/../..)
MODULE_NAME := $(notdir $(CURDIR))
SUBMODULE_PATH_REL := modules/$(MODULE_NAME)/$(SRC_DIR)
PREPARED_MARKER := .prepared

get_source: $(PREPARED_MARKER)

$(PREPARED_MARKER):
	@cd "$(ROOT_DIR)" && \
	if [ -e "$(SUBMODULE_PATH_REL)/.git" ]; then \
		echo "Using module source at $(SUBMODULE_PATH_REL)"; \
	else \
		echo "Initializing module submodule at $(SUBMODULE_PATH_REL)"; \
		git submodule update --init --recursive -- "$(SUBMODULE_PATH_REL)"; \
	fi
	@touch "$@"

clean:
	-$(MAKE) -C $(SRC_DIR) clean
	-rm -f ./*.so

distclean: clean
	-$(MAKE) -C $(SRC_DIR) distclean

pristine:
	@cd "$(ROOT_DIR)" && \
	echo "Deinitializing submodule $(SUBMODULE_PATH_REL) (if initialized)"; \
	git submodule deinit -f -- "$(SUBMODULE_PATH_REL)" >/dev/null 2>&1 || true
	-rm -rf "$(SRC_DIR)" "$(PREPARED_MARKER)"

install: $(TARGET_MODULE)
	mkdir -p $(INSTALL_DIR)
	$(INSTALL) -m 0755 -D $(TARGET_MODULE) $(INSTALL_DIR)

.PHONY: all clean distclean pristine install
