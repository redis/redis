prefix ?= /usr
datadir ?= $(prefix)/share

INSTALL=install

ifneq ($(MAKECMDGOALS),clean)
include ../config-host.mak
endif

CPPFLAGS ?=

override CPPFLAGS += \
	-D_GNU_SOURCE \
	-D__SANE_USERSPACE_TYPES__ \
	-I../src/include/ \
	-include ../config-host.h \
	-D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64

CFLAGS ?= -g -O3 -Wall -Wextra
XCFLAGS = -Wno-unused-parameter -Wno-sign-compare

ifdef CONFIG_HAVE_STRINGOP_OVERFLOW
	XCFLAGS += -Wstringop-overflow=0
endif

ifdef CONFIG_HAVE_ARRAY_BOUNDS
	XCFLAGS += -Warray-bounds=0
endif

CXXFLAGS ?= $(CFLAGS)
override CFLAGS += $(XCFLAGS) -DLIBURING_BUILD_TEST
override CXXFLAGS += $(XCFLAGS) -std=c++11 -DLIBURING_BUILD_TEST

LDFLAGS ?=
override LDFLAGS += -L../src/ -luring -lpthread

# Please keep this list sorted alphabetically.
test_srcs := \
	232c93d07b74.c \
	35fa71a030ca.c \
	500f9fbadef8.c \
	7ad0e4b2f83c.c \
	8a9973408177.c \
	917257daa0fe.c \
	a0908ae19763.c \
	a4c0b3decb33.c \
	accept.c \
	accept-link.c \
	accept-reuse.c \
	accept-test.c \
	across-fork.c \
	b19062a56726.c \
	b5837bd5311d.c \
	buf-ring.c \
	ce593a6c480a.c \
	close-opath.c \
	connect.c \
	connect-rep.c \
	coredump.c \
	cq-full.c \
	cq-overflow.c \
	cq-peek-batch.c \
	cq-ready.c \
	cq-size.c \
	d4ae271dfaae.c \
	d77a67ed5f27.c \
	defer.c \
	defer-taskrun.c \
	double-poll-crash.c \
	drop-submit.c \
	eeed8b54e0df.c \
	empty-eownerdead.c \
	eploop.c \
	eventfd.c \
	eventfd-disable.c \
	eventfd-reg.c \
	eventfd-ring.c \
	evloop.c \
	exec-target.c \
	exit-no-cleanup.c \
	fadvise.c \
	fallocate.c \
	fc2a85cb02ef.c \
	fd-pass.c \
	file-register.c \
	files-exit-hang-poll.c \
	files-exit-hang-timeout.c \
	file-update.c \
	file-verify.c \
	fixed-buf-iter.c \
	fixed-link.c \
	fixed-reuse.c \
	fpos.c \
	fsnotify.c \
	fsync.c \
	hardlink.c \
	io-cancel.c \
	iopoll.c \
	iopoll-leak.c \
	iopoll-overflow.c \
	io_uring_enter.c \
	io_uring_passthrough.c \
	io_uring_register.c \
	io_uring_setup.c \
	lfs-openat.c \
	lfs-openat-write.c \
	link.c \
	link_drain.c \
	link-timeout.c \
	madvise.c \
	mkdir.c \
	msg-ring.c \
	msg-ring-flags.c \
	msg-ring-overflow.c \
	multicqes_drain.c \
	no-mmap-inval.c \
	nolibc.c \
	nop-all-sizes.c \
	nop.c \
	openat2.c \
	open-close.c \
	open-direct-link.c \
	open-direct-pick.c \
	personality.c \
	pipe-bug.c \
	pipe-eof.c \
	pipe-reuse.c \
	poll.c \
	poll-cancel.c \
	poll-cancel-all.c \
	poll-cancel-ton.c \
	poll-link.c \
	poll-many.c \
	poll-mshot-overflow.c \
	poll-mshot-update.c \
	poll-race.c \
	poll-race-mshot.c \
	poll-ring.c \
	poll-v-poll.c \
	pollfree.c \
	probe.c \
	read-before-exit.c \
	read-write.c \
	recv-msgall.c \
	recv-msgall-stream.c \
	recv-multishot.c \
	reg-fd-only.c \
	reg-hint.c \
	reg-reg-ring.c \
	regbuf-merge.c \
	register-restrictions.c \
	rename.c \
	ringbuf-read.c \
	ring-leak2.c \
	ring-leak.c \
	rsrc_tags.c \
	rw_merge_test.c \
	self.c \
	send_recv.c \
	send_recvmsg.c \
	send-zerocopy.c \
	shared-wq.c \
	short-read.c \
	shutdown.c \
	sigfd-deadlock.c \
	single-issuer.c \
	skip-cqe.c \
	socket.c \
	socket-io-cmd.c \
	socket-rw.c \
	socket-rw-eagain.c \
	socket-rw-offset.c \
	splice.c \
	sq-full.c \
	sq-full-cpp.cc \
	sqpoll-cancel-hang.c \
	sqpoll-disable-exit.c \
	sq-poll-dup.c \
	sqpoll-exit-hang.c \
	sq-poll-kthread.c \
	sq-poll-share.c \
	sqpoll-sleep.c \
	sq-space_left.c \
	stdout.c \
	submit-and-wait.c \
	submit-link-fail.c \
	submit-reuse.c \
	symlink.c \
	sync-cancel.c \
	teardowns.c \
	thread-exit.c \
	timeout.c \
	timeout-new.c \
	tty-write-dpoll.c \
	unlink.c \
	version.c \
	wakeup-hang.c \
	wq-aff.c \
	xattr.c \
	# EOL

all_targets :=
include ../Makefile.quiet

ifdef CONFIG_HAVE_STATX
	test_srcs += statx.c
else ifdef CONFIG_HAVE_GLIBC_STATX
	test_srcs += statx.c
endif
all_targets += statx.t

ifdef CONFIG_HAVE_CXX
	test_srcs += sq-full-cpp.cc
endif
all_targets += sq-full-cpp.t


test_targets := $(patsubst %.c,%,$(test_srcs))
test_targets := $(patsubst %.cc,%,$(test_targets))
run_test_targets := $(patsubst %,%.run_test,$(test_targets))
test_targets := $(patsubst %,%.t,$(test_targets))
all_targets += $(test_targets)
helpers = helpers.o

all: $(test_targets)

helpers.o: helpers.c
	$(QUIET_CC)$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ -c $<

LIBURING := $(shell if [ -e ../src/liburing.a ]; then echo ../src/liburing.a; fi)

%.t: %.c $(helpers) helpers.h $(LIBURING)
	$(QUIET_CC)$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $< $(helpers) $(LDFLAGS)

#
# Clang++ is not happy with -Wmissing-prototypes:
#
#   cc1plus: warning: command-line option '-Wmissing-prototypes' \
#   is valid for C/ObjC but not for C++
#
%.t: %.cc $(helpers) helpers.h $(LIBURING)
	$(QUIET_CXX)$(CXX) \
	$(patsubst -Wmissing-prototypes,,$(CPPFLAGS)) \
	$(patsubst -Wmissing-prototypes,,$(CXXFLAGS)) \
	-o $@ $< $(helpers) $(LDFLAGS)


install: $(test_targets) runtests.sh runtests-loop.sh
	$(INSTALL) -D -d -m 755 $(datadir)/liburing-test/
	$(INSTALL) -D -m 755 $(test_targets) $(datadir)/liburing-test/
	$(INSTALL) -D -m 755 runtests.sh  $(datadir)/liburing-test/
	$(INSTALL) -D -m 755 runtests-loop.sh  $(datadir)/liburing-test/

clean:
	@rm -f $(all_targets) helpers.o output/*
	@rm -rf output/

runtests: all
	@./runtests.sh $(test_targets)

runtests-loop: all
	@./runtests-loop.sh $(test_targets)

%.run_test: %.t
	@./runtests-quiet.sh $<

runtests-parallel: $(run_test_targets)
	@echo "All tests passed"

.PHONY: all install clean runtests runtests-loop runtests-parallel
