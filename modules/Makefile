
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
	@RUST_VERSION=1.93.1; \
	ARCH="$$(uname -m)"; \
	if ldd --version 2>&1 | grep -q musl; then LIBC_TYPE="musl"; else LIBC_TYPE="gnu"; fi; \
	echo "Detected architecture: $${ARCH} and libc: $${LIBC_TYPE}"; \
	case "$${ARCH}" in \
		'x86_64') \
			if [ "$${LIBC_TYPE}" = "musl" ]; then \
				RUST_INSTALLER="rust-$${RUST_VERSION}-x86_64-unknown-linux-musl"; \
				RUST_SHA256="5a6c766ce05ad7229aadbb365bad2c9c809c9e378706f123c156821fe6ab2711"; \
			else \
				RUST_INSTALLER="rust-$${RUST_VERSION}-x86_64-unknown-linux-gnu"; \
				RUST_SHA256="fa99eb4e823fdeb8ee25e486c7973b4803013ac68c64e8f74880da788db9739c"; \
			fi ;; \
		'aarch64') \
			if [ "$${LIBC_TYPE}" = "musl" ]; then \
				RUST_INSTALLER="rust-$${RUST_VERSION}-aarch64-unknown-linux-musl"; \
				RUST_SHA256="859d2e973ba1b0106aaeb5df7db72673cb82b9de417339e28ae9b5074756db5b"; \
			else \
				RUST_INSTALLER="rust-$${RUST_VERSION}-aarch64-unknown-linux-gnu"; \
				RUST_SHA256="bbe822f0aae2c1d31fa950a78446d4749e07ed67e974f87bf0c69146df2a9d9c"; \
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
