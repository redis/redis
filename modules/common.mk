PREFIX ?= /usr/local
INSTALL_DIR ?= $(DESTDIR)$(PREFIX)/lib/redis/modules
INSTALL ?= install

# Pin data (repo URL + ref) is loaded from `modules/modules.yaml` via the
# parser in manifest.mk (which itself delegates to scripts/lib/manifest.sh),
# keyed on the basename of the current module directory (e.g. `redisbloom`).
# `?=` keeps the door open for an explicit override in a per-module Makefile.
#
# MODULE_REF / MODULE_REF_KIND resolve the per-module ref in priority order:
#   tag > version > branch > commit  (version is an alias for tag).
# MODULE_VERSION / MODULE_COMMIT remain for backward compatibility with any
# external consumer; the build itself uses MODULE_REF + MODULE_REF_KIND.
include $(dir $(lastword $(MAKEFILE_LIST)))manifest.mk
MODULE_NAME       ?= $(notdir $(CURDIR))
MODULE_REPO       ?= $(call manifest-field,repo,$(MODULE_NAME))
MODULE_REF        ?= $(call manifest-ref,$(MODULE_NAME))
MODULE_REF_KIND   ?= $(call manifest-ref-kind,$(MODULE_NAME))
MODULE_VERSION    ?= $(call manifest-field,version,$(MODULE_NAME))
MODULE_COMMIT     ?= $(call manifest-field,commit,$(MODULE_NAME))
# Build-artifact path under $(SRC_DIR)/bin/$(FULL_VARIANT)/ — from modules.yaml.
MODULE_ARTIFACT   ?= $(call manifest-field,target_module,$(MODULE_NAME))
SRC_DIR           ?= src

# This logic *partially* follows the current module build system. It is a bit awkward and
# should be changed if/when the modules' build process is refactored.

ARCH_MAP_x86_64 := x64
ARCH_MAP_i386 := x86
ARCH_MAP_i686 := x86
ARCH_MAP_aarch64 := arm64v8
ARCH_MAP_arm64 := arm64v8

OS := $(shell uname -s | tr '[:upper:]' '[:lower:]')
# Upstream Redis modules (RedisLabs `readies`/CMake harness) emit build
# artifacts under `bin/macos-<arch>-release/` on macOS, but `uname -s`
# returns "Darwin". Map darwin -> macos so $(TARGET_MODULE) lines up with
# the path the module actually produces.
ifeq ($(OS),darwin)
	OS := macos
endif
ARCH := $(ARCH_MAP_$(shell uname -m))
ifeq ($(ARCH),)
	$(error Unrecognized CPU architecture $(shell uname -m))
endif

FULL_VARIANT := $(OS)-$(ARCH)-release

# Default the build target from the manifest entry. Per-module Makefiles may
# still override TARGET_MODULE by setting it before `include ../common.mk`.
ifeq ($(strip $(TARGET_MODULE)),)
  ifeq ($(strip $(MODULE_ARTIFACT)),)
    $(error No target_module set for $(MODULE_NAME) in modules.yaml)
  endif
  TARGET_MODULE := $(SRC_DIR)/bin/$(FULL_VARIANT)/$(MODULE_ARTIFACT)
endif

# Common rules for all modules, based on per-module configuration

all: $(TARGET_MODULE)

$(TARGET_MODULE): get_source
	$(MAKE) -C $(SRC_DIR)
	cp ${TARGET_MODULE} ./

get_source: $(SRC_DIR)/.prepared

$(SRC_DIR)/.prepared:
	@if [ -d "$(SRC_DIR)/.git" ]; then \
		echo "==> $(SRC_DIR) already cloned, marking prepared (use 'make modules-update $(notdir $(CURDIR))' to refresh)"; \
	elif [ -z "$(MODULE_REF)" ]; then \
		echo "ERROR: no tag/version/branch/commit set for $(MODULE_NAME) in modules.yaml" >&2; exit 1; \
	else \
		mkdir -p $(SRC_DIR); \
		case "$(MODULE_REF_KIND)" in \
		  tag|branch) \
		    git clone --recursive --depth 1 --branch "$(MODULE_REF)" "$(MODULE_REPO)" "$(SRC_DIR)" ;; \
		  commit) \
		    git clone --recursive "$(MODULE_REPO)" "$(SRC_DIR)" && \
		    git -C "$(SRC_DIR)" checkout --detach "$(MODULE_REF)" && \
		    git -C "$(SRC_DIR)" submodule update --init --recursive ;; \
		  *) \
		    echo "ERROR: unknown ref kind '$(MODULE_REF_KIND)' for $(MODULE_NAME)" >&2; exit 1 ;; \
		esac; \
	fi
	@touch $@

clean:
	-$(MAKE) -C $(SRC_DIR) clean
	-rm -f ./*.so

distclean: clean
	-$(MAKE) -C $(SRC_DIR) distclean

pristine:
	-rm -rf $(SRC_DIR)

install: $(TARGET_MODULE)
	mkdir -p $(INSTALL_DIR)
	$(INSTALL) -m 0755 -D $(TARGET_MODULE) $(INSTALL_DIR)

.PHONY: all clean distclean pristine install
