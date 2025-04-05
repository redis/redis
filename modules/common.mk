PREFIX ?= /usr/local
INSTALL_DIR ?= $(DESTDIR)$(PREFIX)/lib/redis/modules
INSTALL ?= install

# This logic *partially* follows the current module build system. It is a bit awkward and
# should be changed if/when the modules' build process is refactored.

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

get_source: $(SRC_DIR)/.prepared

$(SRC_DIR)/.prepared:
	mkdir -p $(SRC_DIR)
	git clone --recursive --depth 1 --branch $(MODULE_VERSION) $(MODULE_REPO) $(SRC_DIR)
	touch $@

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
