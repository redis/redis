
SUBDIRS = redisjson redistimeseries redisbloom redisearch

define submake
	for dir in $(SUBDIRS); do $(MAKE) -C $$dir $(1); done
endef

all: prepare_source
	$(call submake,$@)

get_source:
	$(call submake,$@)

prepare_source: get_source handle-werrors setup_environment

clean:
	$(call submake,$@)

distclean: clean_environment
	$(call submake,$@)

pristine:
	$(call submake,$@)

install:
	$(call submake,$@)

setup_environment: install-rust handle-werrors

clean_environment: uninstall-rust

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

handle-werrors: get_source
ifeq ($(DISABLE_WERRORS),yes)
	@echo "Disabling -Werror for all modules"
	@for dir in $(SUBDIRS); do \
		echo "Processing $$dir"; \
		find $$dir/src -type f \
			\( -name "Makefile" \
			-o -name "*.mk" \
			-o -name "CMakeLists.txt" \) \
			-exec sed -i 's/-Werror//g' {} +; \
	done
endif

.PHONY: all clean distclean install $(SUBDIRS) setup_environment clean_environment install-rust uninstall-rust handle-werrors
