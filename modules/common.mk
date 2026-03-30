PREFIX ?= /usr/local
INSTALL_DIR ?= $(DESTDIR)$(PREFIX)/lib/redis/modules
INSTALL ?= install

ARCH_MAP_x86_64 := x64
ARCH_MAP_i386 := x86
ARCH_MAP_i686 := x86
ARCH_MAP_aarch64 := arm64v8
ARCH_MAP_arm64 := arm64v8

OS := $(shell uname -s | tr '[:upper:]' '[:lower:]')
ifeq ($(OS),darwin)
OS := macos
endif

ARCH := $(ARCH_MAP_$(shell uname -m))
ifeq ($(ARCH),)
	$(error Unrecognized CPU architecture $(shell uname -m))
endif

FULL_VARIANT := $(OS)-$(ARCH)-release

all: $(TARGET_MODULE)

$(TARGET_MODULE): get_source
	$(MAKE) -C $(SRC_DIR) ARCH=$(ARCH)
	cp ${TARGET_MODULE} ./

# Source is managed by utils/modules/update-modules.sh.
# get_source verifies the source tree exists and initializes submodules if
# running inside a git worktree (e.g. after a fresh clone or branch switch).
get_source:
	@if [ -d "$(SRC_DIR)" ]; then \
		echo "Module source present: $(SRC_DIR)"; \
	else \
		echo "ERROR: Module source not found at $(SRC_DIR)." >&2; \
		echo "Run 'utils/modules/update-modules.sh' to clone module sources." >&2; \
		exit 1; \
	fi

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

.PHONY: all clean distclean pristine install get_source
