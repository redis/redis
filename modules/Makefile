MODULES_RUST := redisjson redisearch
MODULES_C := redistimeseries redisbloom redisearch
MODULES_ALL := $(sort $(MODULES_RUST) $(MODULES_C))
.PHONY: $(MODULES_ALL)

MODULES_RUST_GET_SOURCE := $(MODULES_RUST:%=%_get_source)
MODULES_ALL_GET_SOURCE := $(MODULES_ALL:%=%_get_source)
.PHONY: $(MODULES_ALL_GET_SOURCE)

MODULES_RUST_DISTCLEAN := $(MODULES_RUST:%=%_distclean)
MODULES_ALL_DISTCLEAN := $(MODULES_ALL:%=%_distclean)
.PHONY: $(MODULES_ALL_DISTCLEAN)

MODULES_ALL_CLEAN := $(MODULES_ALL:%=%_clean)
.PHONY: $(MODULES_ALL_CLEAN)

MODULES_ALL_PRISTINE := $(MODULES_ALL:%=%_pristine)
.PHONY: $(MODULES_ALL_PRISTINE)

MODULES_ALL_INSTALL := $(MODULES_ALL:%=%_install)
.PHONY: $(MODULES_ALL_INSTALL)

# all
all: $(MODULES_ALL)

$(MODULES_ALL):
	$(MAKE) -C $@ all

# get_source #
# static template for one2one relations: each module requires its own get_source
$(MODULES_ALL): % : %_get_source

# rust modules require installing rust
$(MODULES_RUST_GET_SOURCE): install-rust

$(MODULES_ALL_GET_SOURCE):
	$(MAKE) -C $(@:%_get_source=%) get_source

get_source: $(MODULES_ALL_GET_SOURCE)

# clean #
$(MODULES_ALL_CLEAN):
	$(MAKE) -C $(@:%_clean=%) clean

clean: $(MODULES_ALL_CLEAN)

# distclean #
# rust modules require uninstalling rust on distclean
$(MODULES_RUST_DISTCLEAN): uninstall-rust

$(MODULES_ALL_DISTCLEAN):
	$(MAKE) -C $(@:%_distclean=%) distclean

distclean: $(MODULES_ALL_DISTCLEAN)

# pristine #
$(MODULES_ALL_PRISTINE):
	$(MAKE) -C $(@:%_pristine=%) pristine

pristine: $(MODULES_ALL_PRISTINE)

# install #
# static template for one2one relations: each module install requires its own module build
$(MODULES_ALL_INSTALL): %_install : %

$(MODULES_ALL_INSTALL):
	$(MAKE) -C $(@:%_install=%) install

install: $(MODULES_ALL_INSTALL)


# Keep all of the Rust stuff in one place
# Keep all of the Rust stuff in one place
install-rust:
ifeq ($(INSTALL_RUST_TOOLCHAIN),yes)
	@RUST_VERSION=1.94.0; \
	ARCH="$$(uname -m)"; \
	if ldd --version 2>&1 | grep -q musl; then LIBC_TYPE="musl"; else LIBC_TYPE="gnu"; fi; \
	echo "Detected architecture: $${ARCH} and libc: $${LIBC_TYPE}"; \
	case "$${ARCH}" in \
		'x86_64') \
			if [ "$${LIBC_TYPE}" = "musl" ]; then \
				RUST_INSTALLER="rust-$${RUST_VERSION}-x86_64-unknown-linux-musl"; \
				RUST_SHA256="9a358120ce1491a4d5b7f71a41e4e97b380b5db5d4ec31f7110f5b3090bd3d55"; \
			else \
				RUST_INSTALLER="rust-$${RUST_VERSION}-x86_64-unknown-linux-gnu"; \
				RUST_SHA256="e8fa4185f3ef6ae32725ff638b1ecdbff28f5d651dc0b3111e2539350d03b15a"; \
			fi ;; \
		'aarch64') \
			if [ "$${LIBC_TYPE}" = "musl" ]; then \
				RUST_INSTALLER="rust-$${RUST_VERSION}-aarch64-unknown-linux-musl"; \
				RUST_SHA256="008b3f0fc4175c956ecbfa4e0c48865ec3f953741b2926e75e8ded7e3adfdb19"; \
			else \
				RUST_INSTALLER="rust-$${RUST_VERSION}-aarch64-unknown-linux-gnu"; \
				RUST_SHA256="c6fd6d1c925ed986df3b2c0b89bbc90ce15afb62e4d522a054e7d50c856b3c1a"; \
			fi ;; \
		*) echo >&2 "Unsupported architecture: '$${ARCH}'"; exit 1 ;; \
	esac; \
	echo "Downloading and installing Rust standalone installer: $${RUST_INSTALLER}"; \
	wget --quiet -O $${RUST_INSTALLER}.tar.xz https://static.rust-lang.org/dist/$${RUST_INSTALLER}.tar.xz; \
	echo "$${RUST_SHA256} $${RUST_INSTALLER}.tar.xz" | sha256sum -c --status || { echo "Rust standalone installer checksum failed!"; exit 1; }; \
	tar -xf $${RUST_INSTALLER}.tar.xz; \
	(cd $${RUST_INSTALLER} && ./install.sh); \
	rm -rf $${RUST_INSTALLER}
endif

uninstall-rust:
ifeq ($(INSTALL_RUST_TOOLCHAIN),yes)
	@if [ -x "/usr/local/lib/rustlib/uninstall.sh" ]; then \
		echo "Uninstalling Rust using uninstall.sh script"; \
		rm -rf ~/.cargo; \
		/usr/local/lib/rustlib/uninstall.sh; \
	else \
		echo "WARNING: Rust toolchain not found or uninstall script is missing."; \
	fi
endif

.PHONY: all clean distclean pristine install install-rust uninstall-rust get_source