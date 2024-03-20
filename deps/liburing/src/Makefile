include ../Makefile.common

prefix ?= /usr
includedir ?= $(prefix)/include
libdir ?= $(prefix)/lib
libdevdir ?= $(prefix)/lib

LIBURING_CFLAGS ?=
CPPFLAGS ?=
override CPPFLAGS += -D_GNU_SOURCE \
	-Iinclude/ -include ../config-host.h \
	-D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64
CFLAGS ?= -g -O3 -Wall -Wextra -fno-stack-protector
override CFLAGS += -Wno-unused-parameter \
	-DLIBURING_INTERNAL \
	$(LIBURING_CFLAGS)
SO_CFLAGS=-fPIC $(CFLAGS)
L_CFLAGS=$(CFLAGS)
LINK_FLAGS=-Wl,-z,defs
LINK_FLAGS+=$(LDFLAGS)
ENABLE_SHARED ?= 1

soname=liburing.so.$(VERSION_MAJOR)
libname=liburing.so.$(VERSION)
ffi_soname=liburing-ffi.so.$(VERSION_MAJOR)
ffi_libname=liburing-ffi.so.$(VERSION)

all_targets += liburing.a
all_targets += liburing-ffi.a

ifeq ($(ENABLE_SHARED),1)
all_targets += $(libname)
all_targets += $(ffi_libname)
endif

include ../Makefile.quiet

ifneq ($(MAKECMDGOALS),clean)
include ../config-host.mak
endif

all: $(all_targets)

liburing_srcs := setup.c queue.c register.c syscall.c version.c

ifeq ($(CONFIG_NOLIBC),y)
	liburing_srcs += nolibc.c
	override CFLAGS += -nostdlib -nodefaultlibs -ffreestanding -fno-builtin -fno-stack-protector
	override CPPFLAGS += -nostdlib -nodefaultlibs -ffreestanding -fno-builtin -fno-stack-protector
	override LINK_FLAGS += -nostdlib -nodefaultlibs -lgcc
endif

override CPPFLAGS += -MT "$@" -MMD -MP -MF "$@.d"
liburing_objs := $(patsubst %.c,%.ol,$(liburing_srcs))
liburing_sobjs := $(patsubst %.c,%.os,$(liburing_srcs))
liburing_ffi_objs := ffi.ol
liburing_ffi_sobjs := ffi.os

%.os: %.c
	$(QUIET_CC)$(CC) $(CPPFLAGS) $(SO_CFLAGS) -c -o $@ $<

%.ol: %.c
	$(QUIET_CC)$(CC) $(CPPFLAGS) $(L_CFLAGS) -c -o $@ $<

# Include compiler generated dependency files.
-include $(liburing_objs:%=%.d)
-include $(liburing_sobjs:%=%.d)

AR ?= ar
RANLIB ?= ranlib
liburing.a: $(liburing_objs)
	@rm -f liburing.a
	$(QUIET_AR)$(AR) r liburing.a $^
	$(QUIET_RANLIB)$(RANLIB) liburing.a

liburing-ffi.a: $(liburing_objs) $(liburing_ffi_objs)
	@rm -f liburing-ffi.a
	$(QUIET_AR)$(AR) r liburing-ffi.a $^
	$(QUIET_RANLIB)$(RANLIB) liburing-ffi.a

$(libname): $(liburing_sobjs) liburing.map
	$(QUIET_CC)$(CC) $(SO_CFLAGS) -shared -Wl,--version-script=liburing.map -Wl,-soname=$(soname) -o $@ $(liburing_sobjs) $(LINK_FLAGS)

$(ffi_libname): $(liburing_ffi_objs) $(liburing_ffi_sobjs) $(liburing_sobjs) liburing-ffi.map
	$(QUIET_CC)$(CC) $(SO_CFLAGS) -shared -Wl,--version-script=liburing-ffi.map -Wl,-soname=$(ffi_soname) -o $@ $(liburing_sobjs) $(liburing_ffi_sobjs) $(LINK_FLAGS)

install: $(all_targets)
	install -D -m 644 include/liburing/io_uring.h $(includedir)/liburing/io_uring.h
	install -D -m 644 include/liburing.h $(includedir)/liburing.h
	install -D -m 644 include/liburing/compat.h $(includedir)/liburing/compat.h
	install -D -m 644 include/liburing/barrier.h $(includedir)/liburing/barrier.h
	install -D -m 644 include/liburing/io_uring_version.h $(includedir)/liburing/io_uring_version.h
	install -D -m 644 liburing.a $(libdevdir)/liburing.a
	install -D -m 644 liburing-ffi.a $(libdevdir)/liburing-ffi.a
ifeq ($(ENABLE_SHARED),1)
	install -D -m 755 $(libname) $(libdir)/$(libname)
	install -D -m 755 $(ffi_libname) $(libdir)/$(ffi_libname)
	ln -sf $(libname) $(libdir)/$(soname)
	ln -sf $(relativelibdir)$(libname) $(libdevdir)/liburing.so
	ln -sf $(ffi_libname) $(libdir)/$(ffi_soname)
	ln -sf $(relativelibdir)$(ffi_libname) $(libdevdir)/liburing-ffi.so
endif

clean:
	@rm -f $(all_targets) $(liburing_objs) $(liburing_sobjs) $(liburing_ffi_objs) $(liburing_ffi_sobjs) $(soname).new
	@rm -f *.so* *.a *.o *.d
	@rm -f include/liburing/compat.h
	@rm -f include/liburing/io_uring_version.h

	@# When cleaning, we don't include ../config-host.mak,
	@# so the nolibc objects are always skipped, clean them up!
	@rm -f nolibc.ol nolibc.os
