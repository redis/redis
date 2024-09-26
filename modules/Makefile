
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
	@RUST_VERSION=1.80.1; \
	ARCH="$$(uname -m)"; \
	if ldd --version 2>&1 | grep -q musl; then LIBC_TYPE="musl"; else LIBC_TYPE="gnu"; fi; \
	echo "Detected architecture: $${ARCH} and libc: $${LIBC_TYPE}"; \
	case "$${ARCH}" in \
		'x86_64') \
			if [ "$${LIBC_TYPE}" = "musl" ]; then \
				RUST_INSTALLER="rust-$${RUST_VERSION}-x86_64-unknown-linux-musl"; \
				RUST_SHA256="37bbec6a7b9f55fef79c451260766d281a7a5b9d2e65c348bbc241127cf34c8d"; \
			else \
				RUST_INSTALLER="rust-$${RUST_VERSION}-x86_64-unknown-linux-gnu"; \
				RUST_SHA256="85e936d5d36970afb80756fa122edcc99bd72a88155f6bdd514f5d27e778e00a"; \
			fi ;; \
		'aarch64') \
			if [ "$${LIBC_TYPE}" = "musl" ]; then \
				RUST_INSTALLER="rust-$${RUST_VERSION}-aarch64-unknown-linux-musl"; \
				RUST_SHA256="dd668c2d82f77c5458deb023932600fae633fff8d7f876330e01bc47e9976d17"; \
			else \
				RUST_INSTALLER="rust-$${RUST_VERSION}-aarch64-unknown-linux-gnu"; \
				RUST_SHA256="2e89bad7857711a1c11d017ea28fbfeec54076317763901194f8f5decbac1850"; \
			fi ;; \
		*) echo >&2 "Unsupported architecture: '$${ARCH}'"; exit 1 ;; \
	esac; \
	echo "Downloading and installing Rust standalone installer: $${RUST_INSTALLER}"; \
	wget --quiet -O $${RUST_INSTALLER}.tar.xz https://static.rust-lang.org/dist/$${RUST_INSTALLER}.tar.xz; \
	echo "$${RUST_SHA256} $${RUST_INSTALLER}.tar.xz" | sha256sum -c --quiet || { echo "Rust standalone installer checksum failed!"; exit 1; }; \
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
