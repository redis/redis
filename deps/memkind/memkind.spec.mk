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

# targets for building rpm
version ?= 0.0.0
release ?= 1
arch = $(shell uname -p)
name ?= memkind

rpm: $(name)-$(version).tar.gz
	rpmbuild $(rpmbuild_flags) $^ -ta

memkind-$(version).spec:
	@echo "$$memkind_spec" > $@
	cat ChangeLog >> $@

.PHONY: rpm

define memkind_spec
Summary: User Extensible Heap Manager
Name: $(name)
Version: $(version)
Release: $(release)
License: BSD-2-Clause
Group: System Environment/Libraries
URL: http://memkind.github.io/memkind
BuildRoot: %{_tmppath}/%{name}-%{version}-%{release}-root-%(%{__id_u} -n)
BuildRequires: automake libtool gcc-c++ unzip
%if %{defined suse_version}
BuildRequires: libnuma-devel
%else
BuildRequires: numactl-devel
%endif

Prefix: %{_prefix}
Prefix: %{_unitdir}
Obsoletes: memkind
Provides: memkind

%define namespace memkind

%if %{defined suse_version}
%define docdir %{_defaultdocdir}/%{namespace}
%else
%define docdir %{_defaultdocdir}/%{namespace}-%{version}
%endif

# x86_64 is the only arch memkind will build due to its
# current dependency on SSE4.2 CRC32 instruction which
# is used to compute thread local storage arena mappings
# with polynomial accumulations via GCC's intrinsic _mm_crc32_u64
# For further info check:
# - /lib/gcc/<target>/<version>/include/smmintrin.h
# - https://gcc.gnu.org/bugzilla/show_bug.cgi?id=36095
# - http://en.wikipedia.org/wiki/SSE4
ExclusiveArch: x86_64

# default values if version is a tagged release on github
%{!?commit: %define commit %{version}}
%{!?buildsubdir: %define buildsubdir %{namespace}-%{commit}}
Source0: https://github.com/%{namespace}/%{namespace}/archive/v%{commit}/%{buildsubdir}.tar.gz

%description
The memkind library is an user extensible heap manager built on top of
jemalloc which enables control of memory characteristics and a
partitioning of the heap between kinds of memory. The kinds of memory
are defined by operating system memory policies that have been applied
to virtual address ranges. Memory characteristics supported by
memkind without user extension include control of NUMA and page size
features. The jemalloc non-standard interface has been extended to
enable specialized arenas to make requests for virtual memory from the
operating system through the memkind partition interface. Through the
other memkind interfaces the user can control and extend memory
partition features and allocate memory while selecting enabled
features. This software is being made available for early evaluation.
Feedback on design or implementation is greatly appreciated.

%package devel
Summary: Memkind User Extensible Heap Manager development lib and tools
Group: Development/Libraries
Requires: %{name} = %{version}-%{release}
Obsoletes: memkind-devel
Provides: memkind-devel

%description devel
Install header files and development aids to link memkind library into
applications.

%package tests
Summary: Extention to libnuma for kinds of memory - validation
Group: Validation/Libraries
Requires: %{name} = %{version}-%{release}

%description tests
memkind functional tests

%prep
%setup -q -a 0 -n $(name)-%{version}

%build
# It is required that we configure and build the jemalloc subdirectory
# before we configure and start building the top level memkind directory.
cd %{_builddir}/%{buildsubdir}/jemalloc/
echo %{version} > %{_builddir}/%{buildsubdir}/jemalloc/VERSION
test -f configure || ../build_jemalloc.sh

# Build memkind lib and tools
cd %{_builddir}/%{buildsubdir}
echo %{version} > %{_builddir}/%{buildsubdir}/VERSION
test -f configure || ./autogen.sh
./configure --prefix=%{_prefix} --libdir=%{_libdir} \
           --includedir=%{_includedir} --sbindir=%{_sbindir} --enable-cxx11 \
           --mandir=%{_mandir} --docdir=%{_docdir}/%{namespace}
$(make_prefix)%{__make} %{?_smp_mflags} checkprogs $(make_postfix)


%install
cd %{_builddir}/%{buildsubdir}
%{__make} DESTDIR=%{buildroot} install
%{__install} -d %{buildroot}$(memkind_test_dir)
%{__install} -d %{buildroot}/%{_unitdir}
%{__install} -d %{buildroot}/$(memkind_test_dir)/python_framework
%{__install} test/.libs/* test/*.sh test/*.ts test/*.py %{buildroot}$(memkind_test_dir)
%{__install} test/python_framework/*.py %{buildroot}/$(memkind_test_dir)/python_framework
rm -f %{buildroot}$(memkind_test_dir)/libautohbw.*
rm -f %{buildroot}/%{_libdir}/lib%{namespace}.{l,}a
rm -f %{buildroot}/%{_libdir}/libautohbw.{l,}a
rm -f %{buildroot}/%{_libdir}/lib{numakind}.*

%pre

%post
/sbin/ldconfig

%preun

%postun
/sbin/ldconfig

%files
%defattr(-,root,root,-)
%license %{_docdir}/%{namespace}/COPYING
%doc %{_docdir}/%{namespace}/README
%doc %{_docdir}/%{namespace}/VERSION
%dir %{_docdir}/%{namespace}
%{_libdir}/lib%{namespace}.so.*
%{_libdir}/libautohbw.so.*
%{_bindir}/%{namespace}-hbw-nodes

%define internal_include memkind/internal

%files devel
%defattr(-,root,root,-)
%{_includedir}
%{_includedir}/hbwmalloc.h
%{_includedir}/hbw_allocator.h
%{_libdir}/lib%{namespace}.so
%{_libdir}/libautohbw.so
%{_includedir}/%{namespace}.h
%{_includedir}/%{internal_include}
%{_includedir}/%{internal_include}/%{namespace}*.h
%{_mandir}/man3/hbwmalloc.3.*
%{_mandir}/man3/hbwallocator.3.*
%{_mandir}/man3/%{namespace}*.3.*

%exclude %{_includedir}/%{internal_include}/%{namespace}_log.h

%files tests
%defattr(-,root,root,-)
$(memkind_test_dir)/all_tests
$(memkind_test_dir)/bat_bind_tests
$(memkind_test_dir)/bat_interleave_tests
$(memkind_test_dir)/environ_err_hbw_malloc_test
$(memkind_test_dir)/decorator_test
$(memkind_test_dir)/gb_page_tests_bind_policy
$(memkind_test_dir)/gb_page_tests_preferred_policy
$(memkind_test_dir)/filter_memkind
$(memkind_test_dir)/gb_realloc
$(memkind_test_dir)/hello_hbw
$(memkind_test_dir)/hello_memkind
$(memkind_test_dir)/hello_memkind_debug
$(memkind_test_dir)/memkind_allocated
$(memkind_test_dir)/autohbw_candidates
${memkind_test_dir}/pmem
${memkind_test_dir}/allocator_perf_tool_tests
${memkind_test_dir}/perf_tool
${memkind_test_dir}/autohbw_test_helper
${memkind_test_dir}/trace_mechanism_test_helper
$(memkind_test_dir)/memkind-afts.ts
$(memkind_test_dir)/memkind-afts-ext.ts
$(memkind_test_dir)/memkind-slts.ts
$(memkind_test_dir)/memkind-perf.ts
$(memkind_test_dir)/memkind-perf-ext.ts
$(memkind_test_dir)/memkind-pytests.ts
$(memkind_test_dir)/check.sh
$(memkind_test_dir)/test.sh
$(memkind_test_dir)/hbw_detection_test.py
$(memkind_test_dir)/autohbw_test.py
$(memkind_test_dir)/trace_mechanism_test.py
$(memkind_test_dir)/python_framework
$(memkind_test_dir)/python_framework/cmd_helper.py
$(memkind_test_dir)/python_framework/__init__.py
$(memkind_test_dir)/draw_plots.py
$(memkind_test_dir)/run_alloc_benchmark.sh
$(memkind_test_dir)/alloc_benchmark_hbw
$(memkind_test_dir)/alloc_benchmark_glibc
$(memkind_test_dir)/alloc_benchmark_tbb

%exclude $(memkind_test_dir)/*.pyo
%exclude $(memkind_test_dir)/*.pyc
%exclude $(memkind_test_dir)/python_framework/*.pyo
%exclude $(memkind_test_dir)/python_framework/*.pyc

%changelog
endef

export memkind_spec
