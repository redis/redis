#
#  Copyright (C) 2014 - 2016 Intel Corporation.
#  All rights reserved.
#
#  Redistribution and use in source and binary forms, with or without
#  modification, are permitted provided that the following conditions are met:
#  1. Redistributions of source code must retain the above copyright notice(s),
#     this list of conditions and the following disclaimer.
#  2. Redistributions in binary form must reproduce the above copyright notice(s),
#     this list of conditions and the following disclaimer in the documentation
#     and/or other materials provided with the distribution.
#
#  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER(S) ``AS IS'' AND ANY EXPRESS
#  OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
#  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO
#  EVENT SHALL THE COPYRIGHT HOLDER(S) BE LIABLE FOR ANY DIRECT, INDIRECT,
#  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
#  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
#  PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
#  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
#  OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
#  ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#

ACLOCAL_AMFLAGS = -I m4
AM_CPPFLAGS = -I$(top_srcdir)/include -I$(top_srcdir)/jemalloc/obj/include

lib_LTLIBRARIES = libmemkind.la

libmemkind_la_SOURCES = src/hbwmalloc.c \
                        src/memkind_arena.c \
                        src/memkind.c \
                        src/memkind_default.c \
                        src/memkind_gbtlb.c \
                        src/memkind_hbw.c \
                        src/memkind_hugetlb.c \
                        src/memkind_pmem.c \
                        src/memkind_interleave.c \
                        src/memkind_log.c \
                        # end


libmemkind_la_LIBADD = jemalloc/obj/lib/libjemalloc_pic.a


libmemkind_la_LDFLAGS = -version-info $(memkind_abi_version)

include_HEADERS = include/hbwmalloc.h \
                  include/hbw_allocator.h \
                  include/memkind.h \
                  include/memkind_deprecated.h \
                  # end

nonstandardincludedir = $(includedir)/memkind/internal
nonstandardinclude_HEADERS = include/memkind/internal/memkind_arena.h \
                  include/memkind/internal/memkind_default.h \
                  include/memkind/internal/memkind_gbtlb.h \
                  include/memkind/internal/memkind_hbw.h \
                  include/memkind/internal/memkind_hugetlb.h \
                  include/memkind/internal/memkind_interleave.h \
                  include/memkind/internal/memkind_pmem.h \
                  include/memkind/internal/memkind_private.h \
                  include/memkind/internal/memkind_log.h \
                  # end

sbin_PROGRAMS =

EXTRA_DIST = autogen.sh \
             examples/README \
             test/check.sh \
             test/test.sh \
             VERSION \
             CONTRIBUTING \
             memkind-$(VERSION).spec \
             memkind.spec.mk \
             jemalloc/configure.ac \
             jemalloc/build-aux/config.guess \
             jemalloc/build-aux/install-sh \
             jemalloc/build-aux/config.sub \
             jemalloc/src/util.c \
             jemalloc/src/base.c \
             jemalloc/src/ckh.c \
             jemalloc/src/ctl.c \
             jemalloc/src/ticker.c \
             jemalloc/src/quarantine.c \
             jemalloc/src/rtree.c \
             jemalloc/src/atomic.c \
             jemalloc/src/tcache.c \
             jemalloc/src/pages.c \
             jemalloc/src/tsd.c \
             jemalloc/src/prng.c \
             jemalloc/src/bitmap.c \
             jemalloc/src/nstime.c \
             jemalloc/src/mutex.c \
             jemalloc/src/huge.c \
             jemalloc/src/chunk.c \
             jemalloc/src/witness.c \
             jemalloc/src/extent.c \
             jemalloc/src/prof.c \
             jemalloc/src/jemalloc.c \
             jemalloc/src/hash.c \
             jemalloc/src/mb.c \
             jemalloc/src/chunk_mmap.c \
             jemalloc/src/arena.c \
             jemalloc/src/spin.c \
             jemalloc/src/chunk_dss.c \
             jemalloc/src/stats.c \
             jemalloc/src/zone.c \
             jemalloc/src/valgrind.c \
             jemalloc/include/msvc_compat/C99/stdbool.h \
             jemalloc/include/msvc_compat/C99/stdint.h \
             jemalloc/include/msvc_compat/windows_extra.h \
             jemalloc/include/msvc_compat/strings.h \
             jemalloc/include/jemalloc/jemalloc_macros.h.in \
             jemalloc/include/jemalloc/jemalloc_rename.sh \
             jemalloc/include/jemalloc/jemalloc_protos.h.in \
             jemalloc/include/jemalloc/jemalloc_defs.h.in \
             jemalloc/include/jemalloc/jemalloc.sh \
             jemalloc/include/jemalloc/internal/prng.h \
             jemalloc/include/jemalloc/internal/base.h \
             jemalloc/include/jemalloc/internal/util.h \
             jemalloc/include/jemalloc/internal/private_namespace.sh \
             jemalloc/include/jemalloc/internal/private_symbols.txt \
             jemalloc/include/jemalloc/internal/size_classes.sh \
             jemalloc/include/jemalloc/internal/spin.h \
             jemalloc/include/jemalloc/internal/hash.h \
             jemalloc/include/jemalloc/internal/chunk.h \
             jemalloc/include/jemalloc/internal/public_unnamespace.sh \
             jemalloc/include/jemalloc/internal/bitmap.h \
             jemalloc/include/jemalloc/internal/ctl.h \
             jemalloc/include/jemalloc/internal/extent.h \
             jemalloc/include/jemalloc/internal/jemalloc_internal_macros.h \
             jemalloc/include/jemalloc/internal/rtree.h \
             jemalloc/include/jemalloc/internal/huge.h \
             jemalloc/include/jemalloc/internal/qr.h \
             jemalloc/include/jemalloc/internal/ticker.h \
             jemalloc/include/jemalloc/internal/private_unnamespace.sh \
             jemalloc/include/jemalloc/internal/jemalloc_internal_decls.h \
             jemalloc/include/jemalloc/internal/pages.h \
             jemalloc/include/jemalloc/internal/atomic.h \
             jemalloc/include/jemalloc/internal/valgrind.h \
             jemalloc/include/jemalloc/internal/chunk_dss.h \
             jemalloc/include/jemalloc/internal/witness.h \
             jemalloc/include/jemalloc/internal/assert.h \
             jemalloc/include/jemalloc/internal/ql.h \
             jemalloc/include/jemalloc/internal/mb.h \
             jemalloc/include/jemalloc/internal/tsd.h \
             jemalloc/include/jemalloc/internal/ckh.h \
             jemalloc/include/jemalloc/internal/public_namespace.sh \
             jemalloc/include/jemalloc/internal/tcache.h \
             jemalloc/include/jemalloc/internal/chunk_mmap.h \
             jemalloc/include/jemalloc/internal/stats.h \
             jemalloc/include/jemalloc/internal/smoothstep.sh \
             jemalloc/include/jemalloc/internal/jemalloc_internal.h.in \
             jemalloc/include/jemalloc/internal/arena.h \
             jemalloc/include/jemalloc/internal/mutex.h \
             jemalloc/include/jemalloc/internal/jemalloc_internal_defs.h.in \
             jemalloc/include/jemalloc/internal/quarantine.h \
             jemalloc/include/jemalloc/internal/rb.h \
             jemalloc/include/jemalloc/internal/ph.h \
             jemalloc/include/jemalloc/internal/nstime.h \
             jemalloc/include/jemalloc/internal/prof.h \
             jemalloc/include/jemalloc/internal/smoothstep.h \
             jemalloc/include/jemalloc/jemalloc_mangle.sh \
             jemalloc/include/jemalloc/jemalloc_typedefs.h.in \
             jemalloc/COPYING \
             jemalloc/test/test.sh.in \
             jemalloc/test/src/test.c \
             jemalloc/test/src/btalloc_0.c \
             jemalloc/test/src/mtx.c \
             jemalloc/test/src/SFMT.c \
             jemalloc/test/src/math.c \
             jemalloc/test/src/thd.c \
             jemalloc/test/src/mq.c \
             jemalloc/test/src/timer.c \
             jemalloc/test/src/btalloc_1.c \
             jemalloc/test/src/btalloc.c \
             jemalloc/test/integration/overflow.c \
             jemalloc/test/integration/allocated.c \
             jemalloc/test/integration/thread_arena.c \
             jemalloc/test/integration/xallocx.c \
             jemalloc/test/integration/posix_memalign.c \
             jemalloc/test/integration/sdallocx.c \
             jemalloc/test/integration/aligned_alloc.c \
             jemalloc/test/integration/MALLOCX_ARENA.c \
             jemalloc/test/integration/chunk.c \
             jemalloc/test/integration/rallocx.c \
             jemalloc/test/integration/thread_tcache_enabled.c \
             jemalloc/test/integration/mallocx.c \
             jemalloc/test/include/test/SFMT-params44497.h \
             jemalloc/test/include/test/timer.h \
             jemalloc/test/include/test/math.h \
             jemalloc/test/include/test/SFMT-params.h \
             jemalloc/test/include/test/SFMT-params19937.h \
             jemalloc/test/include/test/SFMT-params2281.h \
             jemalloc/test/include/test/SFMT-params86243.h \
             jemalloc/test/include/test/SFMT-params607.h \
             jemalloc/test/include/test/SFMT.h \
             jemalloc/test/include/test/mtx.h \
             jemalloc/test/include/test/SFMT-sse2.h \
             jemalloc/test/include/test/SFMT-params1279.h \
             jemalloc/test/include/test/SFMT-params216091.h \
             jemalloc/test/include/test/jemalloc_test.h.in \
             jemalloc/test/include/test/SFMT-alti.h \
             jemalloc/test/include/test/thd.h \
             jemalloc/test/include/test/mq.h \
             jemalloc/test/include/test/test.h \
             jemalloc/test/include/test/jemalloc_test_defs.h.in \
             jemalloc/test/include/test/SFMT-params4253.h \
             jemalloc/test/include/test/SFMT-params132049.h \
             jemalloc/test/include/test/btalloc.h \
             jemalloc/test/include/test/SFMT-params11213.h \
             jemalloc/test/unit/util.c \
             jemalloc/test/unit/junk.c \
             jemalloc/test/unit/ckh.c \
             jemalloc/test/unit/prof_gdump.c \
             jemalloc/test/unit/ticker.c \
             jemalloc/test/unit/junk_free.c \
             jemalloc/test/unit/ph.c \
             jemalloc/test/unit/quarantine.c \
             jemalloc/test/unit/rtree.c \
             jemalloc/test/unit/atomic.c \
             jemalloc/test/unit/prof_thread_name.c \
             jemalloc/test/unit/tsd.c \
             jemalloc/test/unit/prof_idump.c \
             jemalloc/test/unit/prof_accum.c \
             jemalloc/test/unit/prof_reset.c \
             jemalloc/test/unit/smoothstep.c \
             jemalloc/test/unit/run_quantize.c \
             jemalloc/test/unit/prng.c \
             jemalloc/test/unit/fork.c \
             jemalloc/test/unit/bitmap.c \
             jemalloc/test/unit/nstime.c \
             jemalloc/test/unit/mtx.c \
             jemalloc/test/unit/arena_reset.c \
             jemalloc/test/unit/SFMT.c \
             jemalloc/test/unit/witness.c \
             jemalloc/test/unit/math.c \
             jemalloc/test/unit/lg_chunk.c \
             jemalloc/test/unit/zero.c \
             jemalloc/test/unit/mq.c \
             jemalloc/test/unit/hash.c \
             jemalloc/test/unit/junk_alloc.c \
             jemalloc/test/unit/size_classes.c \
             jemalloc/test/unit/qr.c \
             jemalloc/test/unit/ql.c \
             jemalloc/test/unit/stats.c \
             jemalloc/test/unit/mallctl.c \
             jemalloc/test/unit/a0.c \
             jemalloc/test/unit/rb.c \
             jemalloc/test/unit/prof_active.c \
             jemalloc/test/unit/decay.c \
             jemalloc/test/stress/microbench.c \
             jemalloc/Makefile.in \
             jemalloc/autogen.sh \
             jemalloc/ChangeLog \
             jemalloc/.autom4te.cfg \
             jemalloc/bin/jeprof.in \
             jemalloc/bin/jemalloc-config.in \
             jemalloc/bin/jemalloc.sh.in \
             jemalloc/msvc/projects/vc2015/test_threads/test_threads.vcxproj.filters \
             jemalloc/msvc/projects/vc2015/test_threads/test_threads.h \
             jemalloc/msvc/projects/vc2015/test_threads/test_threads_main.cpp \
             jemalloc/msvc/projects/vc2015/test_threads/test_threads.vcxproj \
             jemalloc/msvc/projects/vc2015/test_threads/test_threads.cpp \
             jemalloc/msvc/projects/vc2015/jemalloc/jemalloc.vcxproj \
             jemalloc/msvc/projects/vc2015/jemalloc/jemalloc.vcxproj.filters \
             jemalloc/msvc/jemalloc_vc2015.sln \
             jemalloc/msvc/ReadMe.txt \
             jemalloc/coverage.sh \
             jemalloc/jemalloc.pc.in \
             jemalloc/.appveyor.yml \
             jemalloc/README \
             jemalloc/doc/jemalloc.xml.in \
             jemalloc/doc/stylesheet.xsl \
             jemalloc/doc/html.xsl.in \
             jemalloc/doc/manpages.xsl.in \
             jemalloc/INSTALL \
             jemalloc/config.stamp.in \
             jemalloc/.travis.yml \
             build_jemalloc.sh \
             # end

dist_doc_DATA = README COPYING VERSION
dist_man_MANS = man/hbwmalloc.3 \
                man/memkind.3 \
                man/memkind_arena.3 \
                man/memkind_default.3 \
                man/memkind_gbtlb.3 \
                man/memkind_hbw.3 \
                man/memkind_hugetlb.3 \
                man/memkind_pmem.3 \
                man/hbwallocator.3 \
                # end

CLEANFILES = memkind-$(VERSION).spec
DISTCLEANFILES = VERSION

bin_PROGRAMS = memkind-hbw-nodes

memkind_hbw_nodes_SOURCES = src/memkind-hbw-nodes.c
memkind_hbw_nodes_LDADD= libmemkind.la

bin_SCRIPTS =
check_PROGRAMS =
noinst_PROGRAMS =
noinst_LTLIBRARIES =
noinst_HEADERS =

TESTS =
XFAIL_TESTS =
TESTS_ENVIRONMENT =

AM_CFLAGS =  -fvisibility=hidden -Wall -Werror -msse4.2 -D_GNU_SOURCE -DMEMKIND_INTERNAL_API
AM_CXXFLAGS = -fvisibility=hidden -Wall -Werror -msse4.2 -D_GNU_SOURCE -DMEMKIND_INTERNAL_API
if ENABLE_CXX11
AM_CXXFLAGS += -std=c++11
else
AM_CXXFLAGS += -std=c++0x
endif

.PHONY: checkprogs

# create libtool .lo files from jemalloc objects
$(jemalloc_objects): jemalloc/obj/src/%.lo: jemalloc/obj/src/%.pic.o jemalloc/obj/src/%.o
	@echo "# $@ - a libtool object file" > $@
	@echo "# Generated by "`libtool --version | head -n 1` >> $@
	@echo "# Actually generated by memkind build system spoofing libtool" >> $@
	@echo "pic_object='$*.pic.o'" >> $@
	@echo "non_pic_object='$*.o'" >> $@

# build check programs without running tests
checkprogs: $(check_PROGRAMS)

memkind-$(VERSION).spec:
	$(MAKE) version="$(VERSION)" -f memkind.spec.mk $@

rpm: dist
	$(MAKE) version="$(VERSION)" CPPFLAGS="$(CPPFLAGS)" LDFLAGS="$(LDFLAGS)" \
		-f memkind.spec.mk $@

include autohbw/Makefile.mk
include test/Makefile.mk
include examples/Makefile.mk
