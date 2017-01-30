dnl Process this file with autoconf to produce a configure script.
AC_INIT([Makefile.in])

AC_CONFIG_AUX_DIR([build-aux])

dnl ============================================================================
dnl Custom macro definitions.

dnl JE_CFLAGS_APPEND(cflag)
AC_DEFUN([JE_CFLAGS_APPEND],
[
AC_MSG_CHECKING([whether compiler supports $1])
TCFLAGS="${CFLAGS}"
if test "x${CFLAGS}" = "x" ; then
  CFLAGS="$1"
else
  CFLAGS="${CFLAGS} $1"
fi
AC_COMPILE_IFELSE([AC_LANG_PROGRAM(
[[
]], [[
    return 0;
]])],
              [je_cv_cflags_appended=$1]
              AC_MSG_RESULT([yes]),
              [je_cv_cflags_appended=]
              AC_MSG_RESULT([no])
              [CFLAGS="${TCFLAGS}"]
)
])

dnl JE_COMPILABLE(label, hcode, mcode, rvar)
dnl 
dnl Use AC_LINK_IFELSE() rather than AC_COMPILE_IFELSE() so that linker errors
dnl cause failure.
AC_DEFUN([JE_COMPILABLE],
[
AC_CACHE_CHECK([whether $1 is compilable],
               [$4],
               [AC_LINK_IFELSE([AC_LANG_PROGRAM([$2],
                                                [$3])],
                               [$4=yes],
                               [$4=no])])
])

dnl ============================================================================

CONFIG=`echo ${ac_configure_args} | sed -e 's#'"'"'\([^ ]*\)'"'"'#\1#g'`
AC_SUBST([CONFIG])

dnl Library revision.
rev=2
AC_SUBST([rev])

srcroot=$srcdir
if test "x${srcroot}" = "x." ; then
  srcroot=""
else
  srcroot="${srcroot}/"
fi
AC_SUBST([srcroot])
abs_srcroot="`cd \"${srcdir}\"; pwd`/"
AC_SUBST([abs_srcroot])

objroot=""
AC_SUBST([objroot])
abs_objroot="`pwd`/"
AC_SUBST([abs_objroot])

dnl Munge install path variables.
if test "x$prefix" = "xNONE" ; then
  prefix="/usr/local"
fi
if test "x$exec_prefix" = "xNONE" ; then
  exec_prefix=$prefix
fi
PREFIX=$prefix
AC_SUBST([PREFIX])
BINDIR=`eval echo $bindir`
BINDIR=`eval echo $BINDIR`
AC_SUBST([BINDIR])
INCLUDEDIR=`eval echo $includedir`
INCLUDEDIR=`eval echo $INCLUDEDIR`
AC_SUBST([INCLUDEDIR])
LIBDIR=`eval echo $libdir`
LIBDIR=`eval echo $LIBDIR`
AC_SUBST([LIBDIR])
DATADIR=`eval echo $datadir`
DATADIR=`eval echo $DATADIR`
AC_SUBST([DATADIR])
MANDIR=`eval echo $mandir`
MANDIR=`eval echo $MANDIR`
AC_SUBST([MANDIR])

dnl Support for building documentation.
AC_PATH_PROG([XSLTPROC], [xsltproc], [false], [$PATH])
if test -d "/usr/share/xml/docbook/stylesheet/docbook-xsl" ; then
  DEFAULT_XSLROOT="/usr/share/xml/docbook/stylesheet/docbook-xsl"
elif test -d "/usr/share/sgml/docbook/xsl-stylesheets" ; then
  DEFAULT_XSLROOT="/usr/share/sgml/docbook/xsl-stylesheets"
else
  dnl Documentation building will fail if this default gets used.
  DEFAULT_XSLROOT=""
fi
AC_ARG_WITH([xslroot],
  [AS_HELP_STRING([--with-xslroot=<path>], [XSL stylesheet root path])], [
if test "x$with_xslroot" = "xno" ; then
  XSLROOT="${DEFAULT_XSLROOT}"
else
  XSLROOT="${with_xslroot}"
fi
],
  XSLROOT="${DEFAULT_XSLROOT}"
)
AC_SUBST([XSLROOT])

dnl If CFLAGS isn't defined, set CFLAGS to something reasonable.  Otherwise,
dnl just prevent autoconf from molesting CFLAGS.
CFLAGS=$CFLAGS
AC_PROG_CC

if test "x$GCC" != "xyes" ; then
  AC_CACHE_CHECK([whether compiler is MSVC],
                 [je_cv_msvc],
                 [AC_COMPILE_IFELSE([AC_LANG_PROGRAM([],
                                                     [
#ifndef _MSC_VER
  int fail[-1];
#endif
])],
                               [je_cv_msvc=yes],
                               [je_cv_msvc=no])])
fi

dnl check if a cray prgenv wrapper compiler is being used
je_cv_cray_prgenv_wrapper=""
if test "x${PE_ENV}" != "x" ; then
  case "${CC}" in
    CC|cc)
	je_cv_cray_prgenv_wrapper="yes"
	;;
    *)
       ;;
  esac
fi

AC_CACHE_CHECK([whether compiler is cray],
              [je_cv_cray],
              [AC_COMPILE_IFELSE([AC_LANG_PROGRAM([],
                                                  [
#ifndef _CRAYC
  int fail[-1];
#endif
])],
                            [je_cv_cray=yes],
                            [je_cv_cray=no])])

if test "x${je_cv_cray}" = "xyes" ; then
  AC_CACHE_CHECK([whether cray compiler version is 8.4],
                [je_cv_cray_84],
                [AC_COMPILE_IFELSE([AC_LANG_PROGRAM([],
                                                      [
#if !(_RELEASE_MAJOR == 8 && _RELEASE_MINOR == 4)
  int fail[-1];
#endif
])],
                              [je_cv_cray_84=yes],
                              [je_cv_cray_84=no])])
fi

if test "x$CFLAGS" = "x" ; then
  no_CFLAGS="yes"
  if test "x$GCC" = "xyes" ; then
    JE_CFLAGS_APPEND([-std=gnu11])
    if test "x$je_cv_cflags_appended" = "x-std=gnu11" ; then
      AC_DEFINE_UNQUOTED([JEMALLOC_HAS_RESTRICT])
    else
      JE_CFLAGS_APPEND([-std=gnu99])
      if test "x$je_cv_cflags_appended" = "x-std=gnu99" ; then
        AC_DEFINE_UNQUOTED([JEMALLOC_HAS_RESTRICT])
      fi
    fi
    JE_CFLAGS_APPEND([-Wall])
    JE_CFLAGS_APPEND([-Werror=declaration-after-statement])
    JE_CFLAGS_APPEND([-Wshorten-64-to-32])
    JE_CFLAGS_APPEND([-Wsign-compare])
    JE_CFLAGS_APPEND([-pipe])
    JE_CFLAGS_APPEND([-g3])
  elif test "x$je_cv_msvc" = "xyes" ; then
    CC="$CC -nologo"
    JE_CFLAGS_APPEND([-Zi])
    JE_CFLAGS_APPEND([-MT])
    JE_CFLAGS_APPEND([-W3])
    JE_CFLAGS_APPEND([-FS])
    CPPFLAGS="$CPPFLAGS -I${srcdir}/include/msvc_compat"
  fi
  if test "x$je_cv_cray" = "xyes" ; then
    dnl cray compiler 8.4 has an inlining bug
    if test "x$je_cv_cray_84" = "xyes" ; then
      JE_CFLAGS_APPEND([-hipa2])
      JE_CFLAGS_APPEND([-hnognu])
    fi
    if test "x$enable_cc_silence" != "xno" ; then
      dnl ignore unreachable code warning
      JE_CFLAGS_APPEND([-hnomessage=128])
      dnl ignore redefinition of "malloc", "free", etc warning
      JE_CFLAGS_APPEND([-hnomessage=1357])
    fi
  fi
fi
AC_SUBST([EXTRA_CFLAGS])
AC_PROG_CPP

AC_C_BIGENDIAN([ac_cv_big_endian=1], [ac_cv_big_endian=0])
if test "x${ac_cv_big_endian}" = "x1" ; then
  AC_DEFINE_UNQUOTED([JEMALLOC_BIG_ENDIAN], [ ])
fi

if test "x${je_cv_msvc}" = "xyes" -a "x${ac_cv_header_inttypes_h}" = "xno"; then
  CPPFLAGS="$CPPFLAGS -I${srcdir}/include/msvc_compat/C99"
fi

if test "x${je_cv_msvc}" = "xyes" ; then
  LG_SIZEOF_PTR=LG_SIZEOF_PTR_WIN
  AC_MSG_RESULT([Using a predefined value for sizeof(void *): 4 for 32-bit, 8 for 64-bit])
else
  AC_CHECK_SIZEOF([void *])
  if test "x${ac_cv_sizeof_void_p}" = "x8" ; then
    LG_SIZEOF_PTR=3
  elif test "x${ac_cv_sizeof_void_p}" = "x4" ; then
    LG_SIZEOF_PTR=2
  else
    AC_MSG_ERROR([Unsupported pointer size: ${ac_cv_sizeof_void_p}])
  fi
fi
AC_DEFINE_UNQUOTED([LG_SIZEOF_PTR], [$LG_SIZEOF_PTR])

AC_CHECK_SIZEOF([int])
if test "x${ac_cv_sizeof_int}" = "x8" ; then
  LG_SIZEOF_INT=3
elif test "x${ac_cv_sizeof_int}" = "x4" ; then
  LG_SIZEOF_INT=2
else
  AC_MSG_ERROR([Unsupported int size: ${ac_cv_sizeof_int}])
fi
AC_DEFINE_UNQUOTED([LG_SIZEOF_INT], [$LG_SIZEOF_INT])

AC_CHECK_SIZEOF([long])
if test "x${ac_cv_sizeof_long}" = "x8" ; then
  LG_SIZEOF_LONG=3
elif test "x${ac_cv_sizeof_long}" = "x4" ; then
  LG_SIZEOF_LONG=2
else
  AC_MSG_ERROR([Unsupported long size: ${ac_cv_sizeof_long}])
fi
AC_DEFINE_UNQUOTED([LG_SIZEOF_LONG], [$LG_SIZEOF_LONG])

AC_CHECK_SIZEOF([long long])
if test "x${ac_cv_sizeof_long_long}" = "x8" ; then
  LG_SIZEOF_LONG_LONG=3
elif test "x${ac_cv_sizeof_long_long}" = "x4" ; then
  LG_SIZEOF_LONG_LONG=2
else
  AC_MSG_ERROR([Unsupported long long size: ${ac_cv_sizeof_long_long}])
fi
AC_DEFINE_UNQUOTED([LG_SIZEOF_LONG_LONG], [$LG_SIZEOF_LONG_LONG])

AC_CHECK_SIZEOF([intmax_t])
if test "x${ac_cv_sizeof_intmax_t}" = "x16" ; then
  LG_SIZEOF_INTMAX_T=4
elif test "x${ac_cv_sizeof_intmax_t}" = "x8" ; then
  LG_SIZEOF_INTMAX_T=3
elif test "x${ac_cv_sizeof_intmax_t}" = "x4" ; then
  LG_SIZEOF_INTMAX_T=2
else
  AC_MSG_ERROR([Unsupported intmax_t size: ${ac_cv_sizeof_intmax_t}])
fi
AC_DEFINE_UNQUOTED([LG_SIZEOF_INTMAX_T], [$LG_SIZEOF_INTMAX_T])

AC_CANONICAL_HOST
dnl CPU-specific settings.
CPU_SPINWAIT=""
case "${host_cpu}" in
  i686|x86_64)
	if test "x${je_cv_msvc}" = "xyes" ; then
	    AC_CACHE_VAL([je_cv_pause_msvc],
	      [JE_COMPILABLE([pause instruction MSVC], [],
					[[_mm_pause(); return 0;]],
					[je_cv_pause_msvc])])
	    if test "x${je_cv_pause_msvc}" = "xyes" ; then
		CPU_SPINWAIT='_mm_pause()'
	    fi
	else
	    AC_CACHE_VAL([je_cv_pause],
	      [JE_COMPILABLE([pause instruction], [],
					[[__asm__ volatile("pause"); return 0;]],
					[je_cv_pause])])
	    if test "x${je_cv_pause}" = "xyes" ; then
		CPU_SPINWAIT='__asm__ volatile("pause")'
	    fi
	fi
	;;
  powerpc)
	AC_DEFINE_UNQUOTED([HAVE_ALTIVEC], [ ])
	;;
  *)
	;;
esac
AC_DEFINE_UNQUOTED([CPU_SPINWAIT], [$CPU_SPINWAIT])

LD_PRELOAD_VAR="LD_PRELOAD"
so="so"
importlib="${so}"
o="$ac_objext"
a="a"
exe="$ac_exeext"
libprefix="lib"
link_whole_archive="0"
DSO_LDFLAGS='-shared -Wl,-soname,$(@F)'
RPATH='-Wl,-rpath,$(1)'
SOREV="${so}.${rev}"
PIC_CFLAGS='-fPIC -DPIC'
CTARGET='-o $@'
LDTARGET='-o $@'
TEST_LD_MODE=
EXTRA_LDFLAGS=
ARFLAGS='crus'
AROUT=' $@'
CC_MM=1

if test "x$je_cv_cray_prgenv_wrapper" = "xyes" ; then
  TEST_LD_MODE='-dynamic'
fi

if test "x${je_cv_cray}" = "xyes" ; then
  CC_MM=
fi

AN_MAKEVAR([AR], [AC_PROG_AR])
AN_PROGRAM([ar], [AC_PROG_AR])
AC_DEFUN([AC_PROG_AR], [AC_CHECK_TOOL(AR, ar, :)])
AC_PROG_AR

dnl Platform-specific settings.  abi and RPATH can probably be determined
dnl programmatically, but doing so is error-prone, which makes it generally
dnl not worth the trouble.
dnl 
dnl Define cpp macros in CPPFLAGS, rather than doing AC_DEFINE(macro), since the
dnl definitions need to be seen before any headers are included, which is a pain
dnl to make happen otherwise.
CFLAGS="$CFLAGS"
default_munmap="1"
maps_coalesce="1"
case "${host}" in
  *-*-darwin* | *-*-ios*)
	abi="macho"
	RPATH=""
	LD_PRELOAD_VAR="DYLD_INSERT_LIBRARIES"
	so="dylib"
	importlib="${so}"
	force_tls="0"
	DSO_LDFLAGS='-shared -Wl,-install_name,$(LIBDIR)/$(@F)'
	SOREV="${rev}.${so}"
	sbrk_deprecated="1"
	;;
  *-*-freebsd*)
	abi="elf"
	AC_DEFINE([JEMALLOC_SYSCTL_VM_OVERCOMMIT], [ ])
	force_lazy_lock="1"
	;;
  *-*-dragonfly*)
	abi="elf"
	;;
  *-*-openbsd*)
	abi="elf"
	force_tls="0"
	;;
  *-*-bitrig*)
	abi="elf"
	;;
  *-*-linux-android)
	dnl syscall(2) and secure_getenv(3) are exposed by _GNU_SOURCE.
	CPPFLAGS="$CPPFLAGS -D_GNU_SOURCE"
	abi="elf"
	AC_DEFINE([JEMALLOC_HAS_ALLOCA_H])
	AC_DEFINE([JEMALLOC_PROC_SYS_VM_OVERCOMMIT_MEMORY], [ ])
	AC_DEFINE([JEMALLOC_THREADED_INIT], [ ])
	AC_DEFINE([JEMALLOC_C11ATOMICS])
	force_tls="0"
	default_munmap="0"
	;;
  *-*-linux* | *-*-kfreebsd*)
	dnl syscall(2) and secure_getenv(3) are exposed by _GNU_SOURCE.
	CPPFLAGS="$CPPFLAGS -D_GNU_SOURCE"
	abi="elf"
	AC_DEFINE([JEMALLOC_HAS_ALLOCA_H])
	AC_DEFINE([JEMALLOC_PROC_SYS_VM_OVERCOMMIT_MEMORY], [ ])
	AC_DEFINE([JEMALLOC_THREADED_INIT], [ ])
	AC_DEFINE([JEMALLOC_USE_CXX_THROW], [ ])
	default_munmap="0"
	;;
  *-*-netbsd*)
	AC_MSG_CHECKING([ABI])
        AC_COMPILE_IFELSE([AC_LANG_PROGRAM(
[[#ifdef __ELF__
/* ELF */
#else
#error aout
#endif
]])],
                          [abi="elf"],
                          [abi="aout"])
	AC_MSG_RESULT([$abi])
	;;
  *-*-solaris2*)
	abi="elf"
	RPATH='-Wl,-R,$(1)'
	dnl Solaris needs this for sigwait().
	CPPFLAGS="$CPPFLAGS -D_POSIX_PTHREAD_SEMANTICS"
	LIBS="$LIBS -lposix4 -lsocket -lnsl"
	;;
  *-ibm-aix*)
	if "$LG_SIZEOF_PTR" = "8"; then
	  dnl 64bit AIX
	  LD_PRELOAD_VAR="LDR_PRELOAD64"
	else
	  dnl 32bit AIX
	  LD_PRELOAD_VAR="LDR_PRELOAD"
	fi
	abi="xcoff"
	;;
  *-*-mingw* | *-*-cygwin*)
	abi="pecoff"
	force_tls="0"
	maps_coalesce="0"
	RPATH=""
	so="dll"
	if test "x$je_cv_msvc" = "xyes" ; then
	  importlib="lib"
	  DSO_LDFLAGS="-LD"
	  EXTRA_LDFLAGS="-link -DEBUG"
	  CTARGET='-Fo$@'
	  LDTARGET='-Fe$@'
	  AR='lib'
	  ARFLAGS='-nologo -out:'
	  AROUT='$@'
	  CC_MM=
        else
	  importlib="${so}"
	  DSO_LDFLAGS="-shared"
	  link_whole_archive="1"
	fi
	a="lib"
	libprefix=""
	SOREV="${so}"
	PIC_CFLAGS=""
	;;
  *)
	AC_MSG_RESULT([Unsupported operating system: ${host}])
	abi="elf"
	;;
esac

JEMALLOC_USABLE_SIZE_CONST=const
AC_CHECK_HEADERS([malloc.h], [
  AC_MSG_CHECKING([whether malloc_usable_size definition can use const argument])
  AC_COMPILE_IFELSE([AC_LANG_PROGRAM(
    [#include <malloc.h>
     #include <stddef.h>
    size_t malloc_usable_size(const void *ptr);
    ],
    [])],[
                AC_MSG_RESULT([yes])
         ],[
                JEMALLOC_USABLE_SIZE_CONST=
                AC_MSG_RESULT([no])
         ])
])
AC_DEFINE_UNQUOTED([JEMALLOC_USABLE_SIZE_CONST], [$JEMALLOC_USABLE_SIZE_CONST])
AC_SUBST([abi])
AC_SUBST([RPATH])
AC_SUBST([LD_PRELOAD_VAR])
AC_SUBST([so])
AC_SUBST([importlib])
AC_SUBST([o])
AC_SUBST([a])
AC_SUBST([exe])
AC_SUBST([libprefix])
AC_SUBST([link_whole_archive])
AC_SUBST([DSO_LDFLAGS])
AC_SUBST([EXTRA_LDFLAGS])
AC_SUBST([SOREV])
AC_SUBST([PIC_CFLAGS])
AC_SUBST([CTARGET])
AC_SUBST([LDTARGET])
AC_SUBST([TEST_LD_MODE])
AC_SUBST([MKLIB])
AC_SUBST([ARFLAGS])
AC_SUBST([AROUT])
AC_SUBST([CC_MM])

dnl Determine whether libm must be linked to use e.g. log(3).
AC_SEARCH_LIBS([log], [m], , [AC_MSG_ERROR([Missing math functions])])
if test "x$ac_cv_search_log" != "xnone required" ; then
  LM="$ac_cv_search_log"
else
  LM=
fi
AC_SUBST(LM)

JE_COMPILABLE([__attribute__ syntax],
              [static __attribute__((unused)) void foo(void){}],
              [],
              [je_cv_attribute])
if test "x${je_cv_attribute}" = "xyes" ; then
  AC_DEFINE([JEMALLOC_HAVE_ATTR], [ ])
  if test "x${GCC}" = "xyes" -a "x${abi}" = "xelf"; then
    JE_CFLAGS_APPEND([-fvisibility=hidden])
  fi
fi
dnl Check for tls_model attribute support (clang 3.0 still lacks support).
SAVED_CFLAGS="${CFLAGS}"
JE_CFLAGS_APPEND([-Werror])
JE_CFLAGS_APPEND([-herror_on_warning])
JE_COMPILABLE([tls_model attribute], [],
              [static __thread int
               __attribute__((tls_model("initial-exec"), unused)) foo;
               foo = 0;],
              [je_cv_tls_model])
CFLAGS="${SAVED_CFLAGS}"
if test "x${je_cv_tls_model}" = "xyes" ; then
  AC_DEFINE([JEMALLOC_TLS_MODEL],
            [__attribute__((tls_model("initial-exec")))])
else
  AC_DEFINE([JEMALLOC_TLS_MODEL], [ ])
fi
dnl Check for alloc_size attribute support.
SAVED_CFLAGS="${CFLAGS}"
JE_CFLAGS_APPEND([-Werror])
JE_CFLAGS_APPEND([-herror_on_warning])
JE_COMPILABLE([alloc_size attribute], [#include <stdlib.h>],
              [void *foo(size_t size) __attribute__((alloc_size(1)));],
              [je_cv_alloc_size])
CFLAGS="${SAVED_CFLAGS}"
if test "x${je_cv_alloc_size}" = "xyes" ; then
  AC_DEFINE([JEMALLOC_HAVE_ATTR_ALLOC_SIZE], [ ])
fi
dnl Check for format(gnu_printf, ...) attribute support.
SAVED_CFLAGS="${CFLAGS}"
JE_CFLAGS_APPEND([-Werror])
JE_CFLAGS_APPEND([-herror_on_warning])
JE_COMPILABLE([format(gnu_printf, ...) attribute], [#include <stdlib.h>],
              [void *foo(const char *format, ...) __attribute__((format(gnu_printf, 1, 2)));],
              [je_cv_format_gnu_printf])
CFLAGS="${SAVED_CFLAGS}"
if test "x${je_cv_format_gnu_printf}" = "xyes" ; then
  AC_DEFINE([JEMALLOC_HAVE_ATTR_FORMAT_GNU_PRINTF], [ ])
fi
dnl Check for format(printf, ...) attribute support.
SAVED_CFLAGS="${CFLAGS}"
JE_CFLAGS_APPEND([-Werror])
JE_CFLAGS_APPEND([-herror_on_warning])
JE_COMPILABLE([format(printf, ...) attribute], [#include <stdlib.h>],
              [void *foo(const char *format, ...) __attribute__((format(printf, 1, 2)));],
              [je_cv_format_printf])
CFLAGS="${SAVED_CFLAGS}"
if test "x${je_cv_format_printf}" = "xyes" ; then
  AC_DEFINE([JEMALLOC_HAVE_ATTR_FORMAT_PRINTF], [ ])
fi

dnl Support optional additions to rpath.
AC_ARG_WITH([rpath],
  [AS_HELP_STRING([--with-rpath=<rpath>], [Colon-separated rpath (ELF systems only)])],
if test "x$with_rpath" = "xno" ; then
  RPATH_EXTRA=
else
  RPATH_EXTRA="`echo $with_rpath | tr \":\" \" \"`"
fi,
  RPATH_EXTRA=
)
AC_SUBST([RPATH_EXTRA])

dnl Disable rules that do automatic regeneration of configure output by default.
AC_ARG_ENABLE([autogen],
  [AS_HELP_STRING([--enable-autogen], [Automatically regenerate configure output])],
if test "x$enable_autogen" = "xno" ; then
  enable_autogen="0"
else
  enable_autogen="1"
fi
,
enable_autogen="0"
)
AC_SUBST([enable_autogen])

AC_PROG_INSTALL
AC_PROG_RANLIB
AC_PATH_PROG([LD], [ld], [false], [$PATH])
AC_PATH_PROG([AUTOCONF], [autoconf], [false], [$PATH])

public_syms="malloc_conf malloc_message malloc calloc posix_memalign aligned_alloc realloc free mallocx rallocx xallocx sallocx dallocx sdallocx nallocx mallctl mallctlnametomib mallctlbymib malloc_stats_print malloc_usable_size"

dnl Check for allocator-related functions that should be wrapped.
AC_CHECK_FUNC([memalign],
	      [AC_DEFINE([JEMALLOC_OVERRIDE_MEMALIGN], [ ])
	       public_syms="${public_syms} memalign"])
AC_CHECK_FUNC([valloc],
	      [AC_DEFINE([JEMALLOC_OVERRIDE_VALLOC], [ ])
	       public_syms="${public_syms} valloc"])

dnl Do not compute test code coverage by default.
GCOV_FLAGS=
AC_ARG_ENABLE([code-coverage],
  [AS_HELP_STRING([--enable-code-coverage],
   [Enable code coverage])],
[if test "x$enable_code_coverage" = "xno" ; then
  enable_code_coverage="0"
else
  enable_code_coverage="1"
fi
],
[enable_code_coverage="0"]
)
if test "x$enable_code_coverage" = "x1" ; then
  deoptimize="no"
  echo "$CFLAGS $EXTRA_CFLAGS" | grep '\-O' >/dev/null || deoptimize="yes"
  if test "x${deoptimize}" = "xyes" ; then
    JE_CFLAGS_APPEND([-O0])
  fi
  JE_CFLAGS_APPEND([-fprofile-arcs -ftest-coverage])
  EXTRA_LDFLAGS="$EXTRA_LDFLAGS -fprofile-arcs -ftest-coverage"
  AC_DEFINE([JEMALLOC_CODE_COVERAGE], [ ])
fi
AC_SUBST([enable_code_coverage])

dnl Perform no name mangling by default.
AC_ARG_WITH([mangling],
  [AS_HELP_STRING([--with-mangling=<map>], [Mangle symbols in <map>])],
  [mangling_map="$with_mangling"], [mangling_map=""])

dnl Do not prefix public APIs by default.
AC_ARG_WITH([jemalloc_prefix],
  [AS_HELP_STRING([--with-jemalloc-prefix=<prefix>], [Prefix to prepend to all public APIs])],
  [JEMALLOC_PREFIX="$with_jemalloc_prefix"],
  [if test "x$abi" != "xmacho" -a "x$abi" != "xpecoff"; then
  JEMALLOC_PREFIX=""
else
  JEMALLOC_PREFIX="je_"
fi]
)
if test "x$JEMALLOC_PREFIX" != "x" ; then
  JEMALLOC_CPREFIX=`echo ${JEMALLOC_PREFIX} | tr "a-z" "A-Z"`
  AC_DEFINE_UNQUOTED([JEMALLOC_PREFIX], ["$JEMALLOC_PREFIX"])
  AC_DEFINE_UNQUOTED([JEMALLOC_CPREFIX], ["$JEMALLOC_CPREFIX"])
fi
AC_SUBST([JEMALLOC_CPREFIX])

AC_ARG_WITH([export],
  [AS_HELP_STRING([--without-export], [disable exporting jemalloc public APIs])],
  [if test "x$with_export" = "xno"; then
  AC_DEFINE([JEMALLOC_EXPORT],[])
fi]
)

dnl Mangle library-private APIs.
AC_ARG_WITH([private_namespace],
  [AS_HELP_STRING([--with-private-namespace=<prefix>], [Prefix to prepend to all library-private APIs])],
  [JEMALLOC_PRIVATE_NAMESPACE="${with_private_namespace}je_"],
  [JEMALLOC_PRIVATE_NAMESPACE="je_"]
)
AC_DEFINE_UNQUOTED([JEMALLOC_PRIVATE_NAMESPACE], [$JEMALLOC_PRIVATE_NAMESPACE])
private_namespace="$JEMALLOC_PRIVATE_NAMESPACE"
AC_SUBST([private_namespace])

dnl Do not add suffix to installed files by default.
AC_ARG_WITH([install_suffix],
  [AS_HELP_STRING([--with-install-suffix=<suffix>], [Suffix to append to all installed files])],
  [INSTALL_SUFFIX="$with_install_suffix"],
  [INSTALL_SUFFIX=]
)
install_suffix="$INSTALL_SUFFIX"
AC_SUBST([install_suffix])

dnl Specify default malloc_conf.
AC_ARG_WITH([malloc_conf],
  [AS_HELP_STRING([--with-malloc-conf=<malloc_conf>], [config.malloc_conf options string])],
  [JEMALLOC_CONFIG_MALLOC_CONF="$with_malloc_conf"],
  [JEMALLOC_CONFIG_MALLOC_CONF=""]
)
config_malloc_conf="$JEMALLOC_CONFIG_MALLOC_CONF"
AC_DEFINE_UNQUOTED([JEMALLOC_CONFIG_MALLOC_CONF], ["$config_malloc_conf"])

dnl Substitute @je_@ in jemalloc_protos.h.in, primarily to make generation of
dnl jemalloc_protos_jet.h easy.
je_="je_"
AC_SUBST([je_])

cfgoutputs_in="Makefile.in"
cfgoutputs_in="${cfgoutputs_in} jemalloc.pc.in"
cfgoutputs_in="${cfgoutputs_in} doc/html.xsl.in"
cfgoutputs_in="${cfgoutputs_in} doc/manpages.xsl.in"
cfgoutputs_in="${cfgoutputs_in} doc/jemalloc.xml.in"
cfgoutputs_in="${cfgoutputs_in} include/jemalloc/jemalloc_macros.h.in"
cfgoutputs_in="${cfgoutputs_in} include/jemalloc/jemalloc_protos.h.in"
cfgoutputs_in="${cfgoutputs_in} include/jemalloc/jemalloc_typedefs.h.in"
cfgoutputs_in="${cfgoutputs_in} include/jemalloc/internal/jemalloc_internal.h.in"
cfgoutputs_in="${cfgoutputs_in} test/test.sh.in"
cfgoutputs_in="${cfgoutputs_in} test/include/test/jemalloc_test.h.in"

cfgoutputs_out="Makefile"
cfgoutputs_out="${cfgoutputs_out} jemalloc.pc"
cfgoutputs_out="${cfgoutputs_out} doc/html.xsl"
cfgoutputs_out="${cfgoutputs_out} doc/manpages.xsl"
cfgoutputs_out="${cfgoutputs_out} doc/jemalloc.xml"
cfgoutputs_out="${cfgoutputs_out} include/jemalloc/jemalloc_macros.h"
cfgoutputs_out="${cfgoutputs_out} include/jemalloc/jemalloc_protos.h"
cfgoutputs_out="${cfgoutputs_out} include/jemalloc/jemalloc_typedefs.h"
cfgoutputs_out="${cfgoutputs_out} include/jemalloc/internal/jemalloc_internal.h"
cfgoutputs_out="${cfgoutputs_out} test/test.sh"
cfgoutputs_out="${cfgoutputs_out} test/include/test/jemalloc_test.h"

cfgoutputs_tup="Makefile"
cfgoutputs_tup="${cfgoutputs_tup} jemalloc.pc:jemalloc.pc.in"
cfgoutputs_tup="${cfgoutputs_tup} doc/html.xsl:doc/html.xsl.in"
cfgoutputs_tup="${cfgoutputs_tup} doc/manpages.xsl:doc/manpages.xsl.in"
cfgoutputs_tup="${cfgoutputs_tup} doc/jemalloc.xml:doc/jemalloc.xml.in"
cfgoutputs_tup="${cfgoutputs_tup} include/jemalloc/jemalloc_macros.h:include/jemalloc/jemalloc_macros.h.in"
cfgoutputs_tup="${cfgoutputs_tup} include/jemalloc/jemalloc_protos.h:include/jemalloc/jemalloc_protos.h.in"
cfgoutputs_tup="${cfgoutputs_tup} include/jemalloc/jemalloc_typedefs.h:include/jemalloc/jemalloc_typedefs.h.in"
cfgoutputs_tup="${cfgoutputs_tup} include/jemalloc/internal/jemalloc_internal.h"
cfgoutputs_tup="${cfgoutputs_tup} test/test.sh:test/test.sh.in"
cfgoutputs_tup="${cfgoutputs_tup} test/include/test/jemalloc_test.h:test/include/test/jemalloc_test.h.in"

cfghdrs_in="include/jemalloc/jemalloc_defs.h.in"
cfghdrs_in="${cfghdrs_in} include/jemalloc/internal/jemalloc_internal_defs.h.in"
cfghdrs_in="${cfghdrs_in} include/jemalloc/internal/private_namespace.sh"
cfghdrs_in="${cfghdrs_in} include/jemalloc/internal/private_unnamespace.sh"
cfghdrs_in="${cfghdrs_in} include/jemalloc/internal/private_symbols.txt"
cfghdrs_in="${cfghdrs_in} include/jemalloc/internal/public_namespace.sh"
cfghdrs_in="${cfghdrs_in} include/jemalloc/internal/public_unnamespace.sh"
cfghdrs_in="${cfghdrs_in} include/jemalloc/internal/size_classes.sh"
cfghdrs_in="${cfghdrs_in} include/jemalloc/jemalloc_rename.sh"
cfghdrs_in="${cfghdrs_in} include/jemalloc/jemalloc_mangle.sh"
cfghdrs_in="${cfghdrs_in} include/jemalloc/jemalloc.sh"
cfghdrs_in="${cfghdrs_in} test/include/test/jemalloc_test_defs.h.in"

cfghdrs_out="include/jemalloc/jemalloc_defs.h"
cfghdrs_out="${cfghdrs_out} include/jemalloc/jemalloc${install_suffix}.h"
cfghdrs_out="${cfghdrs_out} include/jemalloc/internal/private_namespace.h"
cfghdrs_out="${cfghdrs_out} include/jemalloc/internal/private_unnamespace.h"
cfghdrs_out="${cfghdrs_out} include/jemalloc/internal/public_symbols.txt"
cfghdrs_out="${cfghdrs_out} include/jemalloc/internal/public_namespace.h"
cfghdrs_out="${cfghdrs_out} include/jemalloc/internal/public_unnamespace.h"
cfghdrs_out="${cfghdrs_out} include/jemalloc/internal/size_classes.h"
cfghdrs_out="${cfghdrs_out} include/jemalloc/jemalloc_protos_jet.h"
cfghdrs_out="${cfghdrs_out} include/jemalloc/jemalloc_rename.h"
cfghdrs_out="${cfghdrs_out} include/jemalloc/jemalloc_mangle.h"
cfghdrs_out="${cfghdrs_out} include/jemalloc/jemalloc_mangle_jet.h"
cfghdrs_out="${cfghdrs_out} include/jemalloc/internal/jemalloc_internal_defs.h"
cfghdrs_out="${cfghdrs_out} test/include/test/jemalloc_test_defs.h"

cfghdrs_tup="include/jemalloc/jemalloc_defs.h:include/jemalloc/jemalloc_defs.h.in"
cfghdrs_tup="${cfghdrs_tup} include/jemalloc/internal/jemalloc_internal_defs.h:include/jemalloc/internal/jemalloc_internal_defs.h.in"
cfghdrs_tup="${cfghdrs_tup} test/include/test/jemalloc_test_defs.h:test/include/test/jemalloc_test_defs.h.in"

dnl Silence irrelevant compiler warnings by default.
AC_ARG_ENABLE([cc-silence],
  [AS_HELP_STRING([--disable-cc-silence],
                  [Do not silence irrelevant compiler warnings])],
[if test "x$enable_cc_silence" = "xno" ; then
  enable_cc_silence="0"
else
  enable_cc_silence="1"
fi
],
[enable_cc_silence="1"]
)
if test "x$enable_cc_silence" = "x1" ; then
  AC_DEFINE([JEMALLOC_CC_SILENCE], [ ])
fi

dnl Do not compile with debugging by default.
AC_ARG_ENABLE([debug],
  [AS_HELP_STRING([--enable-debug],
                  [Build debugging code (implies --enable-ivsalloc)])],
[if test "x$enable_debug" = "xno" ; then
  enable_debug="0"
else
  enable_debug="1"
fi
],
[enable_debug="0"]
)
if test "x$enable_debug" = "x1" ; then
  AC_DEFINE([JEMALLOC_DEBUG], [ ])
fi
if test "x$enable_debug" = "x1" ; then
  AC_DEFINE([JEMALLOC_DEBUG], [ ])
  enable_ivsalloc="1"
fi
AC_SUBST([enable_debug])

dnl Do not validate pointers by default.
AC_ARG_ENABLE([ivsalloc],
  [AS_HELP_STRING([--enable-ivsalloc],
                  [Validate pointers passed through the public API])],
[if test "x$enable_ivsalloc" = "xno" ; then
  enable_ivsalloc="0"
else
  enable_ivsalloc="1"
fi
],
[enable_ivsalloc="0"]
)
if test "x$enable_ivsalloc" = "x1" ; then
  AC_DEFINE([JEMALLOC_IVSALLOC], [ ])
fi

dnl Only optimize if not debugging.
if test "x$enable_debug" = "x0" -a "x$no_CFLAGS" = "xyes" ; then
  dnl Make sure that an optimization flag was not specified in EXTRA_CFLAGS.
  optimize="no"
  echo "$CFLAGS $EXTRA_CFLAGS" | grep '\-O' >/dev/null || optimize="yes"
  if test "x${optimize}" = "xyes" ; then
    if test "x$GCC" = "xyes" ; then
      JE_CFLAGS_APPEND([-O3])
      JE_CFLAGS_APPEND([-funroll-loops])
    elif test "x$je_cv_msvc" = "xyes" ; then
      JE_CFLAGS_APPEND([-O2])
    else
      JE_CFLAGS_APPEND([-O])
    fi
  fi
fi

dnl Enable statistics calculation by default.
AC_ARG_ENABLE([stats],
  [AS_HELP_STRING([--disable-stats],
                  [Disable statistics calculation/reporting])],
[if test "x$enable_stats" = "xno" ; then
  enable_stats="0"
else
  enable_stats="1"
fi
],
[enable_stats="1"]
)
if test "x$enable_stats" = "x1" ; then
  AC_DEFINE([JEMALLOC_STATS], [ ])
fi
AC_SUBST([enable_stats])

dnl Do not enable profiling by default.
AC_ARG_ENABLE([prof],
  [AS_HELP_STRING([--enable-prof], [Enable allocation profiling])],
[if test "x$enable_prof" = "xno" ; then
  enable_prof="0"
else
  enable_prof="1"
fi
],
[enable_prof="0"]
)
if test "x$enable_prof" = "x1" ; then
  backtrace_method=""
else
  backtrace_method="N/A"
fi

AC_ARG_ENABLE([prof-libunwind],
  [AS_HELP_STRING([--enable-prof-libunwind], [Use libunwind for backtracing])],
[if test "x$enable_prof_libunwind" = "xno" ; then
  enable_prof_libunwind="0"
else
  enable_prof_libunwind="1"
fi
],
[enable_prof_libunwind="0"]
)
AC_ARG_WITH([static_libunwind],
  [AS_HELP_STRING([--with-static-libunwind=<libunwind.a>],
  [Path to static libunwind library; use rather than dynamically linking])],
if test "x$with_static_libunwind" = "xno" ; then
  LUNWIND="-lunwind"
else
  if test ! -f "$with_static_libunwind" ; then
    AC_MSG_ERROR([Static libunwind not found: $with_static_libunwind])
  fi
  LUNWIND="$with_static_libunwind"
fi,
  LUNWIND="-lunwind"
)
if test "x$backtrace_method" = "x" -a "x$enable_prof_libunwind" = "x1" ; then
  AC_CHECK_HEADERS([libunwind.h], , [enable_prof_libunwind="0"])
  if test "x$LUNWIND" = "x-lunwind" ; then
    AC_CHECK_LIB([unwind], [unw_backtrace], [LIBS="$LIBS $LUNWIND"],
                 [enable_prof_libunwind="0"])
  else
    LIBS="$LIBS $LUNWIND"
  fi
  if test "x${enable_prof_libunwind}" = "x1" ; then
    backtrace_method="libunwind"
    AC_DEFINE([JEMALLOC_PROF_LIBUNWIND], [ ])
  fi
fi

AC_ARG_ENABLE([prof-libgcc],
  [AS_HELP_STRING([--disable-prof-libgcc],
  [Do not use libgcc for backtracing])],
[if test "x$enable_prof_libgcc" = "xno" ; then
  enable_prof_libgcc="0"
else
  enable_prof_libgcc="1"
fi
],
[enable_prof_libgcc="1"]
)
if test "x$backtrace_method" = "x" -a "x$enable_prof_libgcc" = "x1" \
     -a "x$GCC" = "xyes" ; then
  AC_CHECK_HEADERS([unwind.h], , [enable_prof_libgcc="0"])
  AC_CHECK_LIB([gcc], [_Unwind_Backtrace], [LIBS="$LIBS -lgcc"], [enable_prof_libgcc="0"])
  if test "x${enable_prof_libgcc}" = "x1" ; then
    backtrace_method="libgcc"
    AC_DEFINE([JEMALLOC_PROF_LIBGCC], [ ])
  fi
else
  enable_prof_libgcc="0"
fi

AC_ARG_ENABLE([prof-gcc],
  [AS_HELP_STRING([--disable-prof-gcc],
  [Do not use gcc intrinsics for backtracing])],
[if test "x$enable_prof_gcc" = "xno" ; then
  enable_prof_gcc="0"
else
  enable_prof_gcc="1"
fi
],
[enable_prof_gcc="1"]
)
if test "x$backtrace_method" = "x" -a "x$enable_prof_gcc" = "x1" \
     -a "x$GCC" = "xyes" ; then
  JE_CFLAGS_APPEND([-fno-omit-frame-pointer])
  backtrace_method="gcc intrinsics"
  AC_DEFINE([JEMALLOC_PROF_GCC], [ ])
else
  enable_prof_gcc="0"
fi

if test "x$backtrace_method" = "x" ; then
  backtrace_method="none (disabling profiling)"
  enable_prof="0"
fi
AC_MSG_CHECKING([configured backtracing method])
AC_MSG_RESULT([$backtrace_method])
if test "x$enable_prof" = "x1" ; then
  dnl Heap profiling uses the log(3) function.
  if test "x$LM" != "x" ; then
    LIBS="$LIBS $LM"
  fi

  AC_DEFINE([JEMALLOC_PROF], [ ])
fi
AC_SUBST([enable_prof])

dnl Enable thread-specific caching by default.
AC_ARG_ENABLE([tcache],
  [AS_HELP_STRING([--disable-tcache], [Disable per thread caches])],
[if test "x$enable_tcache" = "xno" ; then
  enable_tcache="0"
else
  enable_tcache="1"
fi
],
[enable_tcache="1"]
)
if test "x$enable_tcache" = "x1" ; then
  AC_DEFINE([JEMALLOC_TCACHE], [ ])
fi
AC_SUBST([enable_tcache])

dnl Indicate whether adjacent virtual memory mappings automatically coalesce
dnl (and fragment on demand).
if test "x${maps_coalesce}" = "x1" ; then
  AC_DEFINE([JEMALLOC_MAPS_COALESCE], [ ])
fi

dnl Enable VM deallocation via munmap() by default.
AC_ARG_ENABLE([munmap],
  [AS_HELP_STRING([--disable-munmap], [Disable VM deallocation via munmap(2)])],
[if test "x$enable_munmap" = "xno" ; then
  enable_munmap="0"
else
  enable_munmap="1"
fi
],
[enable_munmap="${default_munmap}"]
)
if test "x$enable_munmap" = "x1" ; then
  AC_DEFINE([JEMALLOC_MUNMAP], [ ])
fi
AC_SUBST([enable_munmap])

dnl Enable allocation from DSS if supported by the OS.
have_dss="1"
dnl Check whether the BSD/SUSv1 sbrk() exists.  If not, disable DSS support.
AC_CHECK_FUNC([sbrk], [have_sbrk="1"], [have_sbrk="0"])
if test "x$have_sbrk" = "x1" ; then
  if test "x$sbrk_deprecated" = "x1" ; then
    AC_MSG_RESULT([Disabling dss allocation because sbrk is deprecated])
    have_dss="0"
  fi
else
  have_dss="0"
fi

if test "x$have_dss" = "x1" ; then
  AC_DEFINE([JEMALLOC_DSS], [ ])
fi

dnl Support the junk/zero filling option by default.
AC_ARG_ENABLE([fill],
  [AS_HELP_STRING([--disable-fill],
                  [Disable support for junk/zero filling, quarantine, and redzones])],
[if test "x$enable_fill" = "xno" ; then
  enable_fill="0"
else
  enable_fill="1"
fi
],
[enable_fill="1"]
)
if test "x$enable_fill" = "x1" ; then
  AC_DEFINE([JEMALLOC_FILL], [ ])
fi
AC_SUBST([enable_fill])

dnl Disable utrace(2)-based tracing by default.
AC_ARG_ENABLE([utrace],
  [AS_HELP_STRING([--enable-utrace], [Enable utrace(2)-based tracing])],
[if test "x$enable_utrace" = "xno" ; then
  enable_utrace="0"
else
  enable_utrace="1"
fi
],
[enable_utrace="0"]
)
JE_COMPILABLE([utrace(2)], [
#include <sys/types.h>
#include <sys/param.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/ktrace.h>
], [
	utrace((void *)0, 0);
], [je_cv_utrace])
if test "x${je_cv_utrace}" = "xno" ; then
  enable_utrace="0"
fi
if test "x$enable_utrace" = "x1" ; then
  AC_DEFINE([JEMALLOC_UTRACE], [ ])
fi
AC_SUBST([enable_utrace])

dnl Support Valgrind by default.
AC_ARG_ENABLE([valgrind],
  [AS_HELP_STRING([--disable-valgrind], [Disable support for Valgrind])],
[if test "x$enable_valgrind" = "xno" ; then
  enable_valgrind="0"
else
  enable_valgrind="1"
fi
],
[enable_valgrind="1"]
)
if test "x$enable_valgrind" = "x1" ; then
  JE_COMPILABLE([valgrind], [
#include <valgrind/valgrind.h>
#include <valgrind/memcheck.h>

#if !defined(VALGRIND_RESIZEINPLACE_BLOCK)
#  error "Incompatible Valgrind version"
#endif
], [], [je_cv_valgrind])
  if test "x${je_cv_valgrind}" = "xno" ; then
    enable_valgrind="0"
  fi
  if test "x$enable_valgrind" = "x1" ; then
    AC_DEFINE([JEMALLOC_VALGRIND], [ ])
  fi
fi
AC_SUBST([enable_valgrind])

dnl Do not support the xmalloc option by default.
AC_ARG_ENABLE([xmalloc],
  [AS_HELP_STRING([--enable-xmalloc], [Support xmalloc option])],
[if test "x$enable_xmalloc" = "xno" ; then
  enable_xmalloc="0"
else
  enable_xmalloc="1"
fi
],
[enable_xmalloc="0"]
)
if test "x$enable_xmalloc" = "x1" ; then
  AC_DEFINE([JEMALLOC_XMALLOC], [ ])
fi
AC_SUBST([enable_xmalloc])

dnl Support cache-oblivious allocation alignment by default.
AC_ARG_ENABLE([cache-oblivious],
  [AS_HELP_STRING([--disable-cache-oblivious],
                  [Disable support for cache-oblivious allocation alignment])],
[if test "x$enable_cache_oblivious" = "xno" ; then
  enable_cache_oblivious="0"
else
  enable_cache_oblivious="1"
fi
],
[enable_cache_oblivious="1"]
)
if test "x$enable_cache_oblivious" = "x1" ; then
  AC_DEFINE([JEMALLOC_CACHE_OBLIVIOUS], [ ])
fi
AC_SUBST([enable_cache_oblivious])



JE_COMPILABLE([a program using __builtin_unreachable], [
void foo (void) {
  __builtin_unreachable();
}
], [
	{
		foo();
	}
], [je_cv_gcc_builtin_unreachable])
if test "x${je_cv_gcc_builtin_unreachable}" = "xyes" ; then
  AC_DEFINE([JEMALLOC_INTERNAL_UNREACHABLE], [__builtin_unreachable])
else
  AC_DEFINE([JEMALLOC_INTERNAL_UNREACHABLE], [abort])
fi

dnl ============================================================================
dnl Check for  __builtin_ffsl(), then ffsl(3), and fail if neither are found.
dnl One of those two functions should (theoretically) exist on all platforms
dnl that jemalloc currently has a chance of functioning on without modification.
dnl We additionally assume ffs[ll]() or __builtin_ffs[ll]() are defined if
dnl ffsl() or __builtin_ffsl() are defined, respectively.
JE_COMPILABLE([a program using __builtin_ffsl], [
#include <stdio.h>
#include <strings.h>
#include <string.h>
], [
	{
		int rv = __builtin_ffsl(0x08);
		printf("%d\n", rv);
	}
], [je_cv_gcc_builtin_ffsl])
if test "x${je_cv_gcc_builtin_ffsl}" = "xyes" ; then
  AC_DEFINE([JEMALLOC_INTERNAL_FFSLL], [__builtin_ffsll])
  AC_DEFINE([JEMALLOC_INTERNAL_FFSL], [__builtin_ffsl])
  AC_DEFINE([JEMALLOC_INTERNAL_FFS], [__builtin_ffs])
else
  JE_COMPILABLE([a program using ffsl], [
  #include <stdio.h>
  #include <strings.h>
  #include <string.h>
  ], [
	{
		int rv = ffsl(0x08);
		printf("%d\n", rv);
	}
  ], [je_cv_function_ffsl])
  if test "x${je_cv_function_ffsl}" = "xyes" ; then
    AC_DEFINE([JEMALLOC_INTERNAL_FFSLL], [ffsll])
    AC_DEFINE([JEMALLOC_INTERNAL_FFSL], [ffsl])
    AC_DEFINE([JEMALLOC_INTERNAL_FFS], [ffs])
  else
    AC_MSG_ERROR([Cannot build without ffsl(3) or __builtin_ffsl()])
  fi
fi

AC_ARG_WITH([lg_tiny_min],
  [AS_HELP_STRING([--with-lg-tiny-min=<lg-tiny-min>],
   [Base 2 log of minimum tiny size class to support])],
  [LG_TINY_MIN="$with_lg_tiny_min"],
  [LG_TINY_MIN="3"])
AC_DEFINE_UNQUOTED([LG_TINY_MIN], [$LG_TINY_MIN])

AC_ARG_WITH([lg_quantum],
  [AS_HELP_STRING([--with-lg-quantum=<lg-quantum>],
   [Base 2 log of minimum allocation alignment])],
  [LG_QUANTA="$with_lg_quantum"],
  [LG_QUANTA="3 4"])
if test "x$with_lg_quantum" != "x" ; then
  AC_DEFINE_UNQUOTED([LG_QUANTUM], [$with_lg_quantum])
fi

AC_ARG_WITH([lg_page],
  [AS_HELP_STRING([--with-lg-page=<lg-page>], [Base 2 log of system page size])],
  [LG_PAGE="$with_lg_page"], [LG_PAGE="detect"])
if test "x$LG_PAGE" = "xdetect"; then
  AC_CACHE_CHECK([LG_PAGE],
               [je_cv_lg_page],
               AC_RUN_IFELSE([AC_LANG_PROGRAM(
[[
#include <strings.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif
#include <stdio.h>
]],
[[
    int result;
    FILE *f;

#ifdef _WIN32
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    result = si.dwPageSize;
#else
    result = sysconf(_SC_PAGESIZE);
#endif
    if (result == -1) {
	return 1;
    }
    result = JEMALLOC_INTERNAL_FFSL(result) - 1;

    f = fopen("conftest.out", "w");
    if (f == NULL) {
	return 1;
    }
    fprintf(f, "%d", result);
    fclose(f);

    return 0;
]])],
                             [je_cv_lg_page=`cat conftest.out`],
                             [je_cv_lg_page=undefined],
                             [je_cv_lg_page=12]))
fi
if test "x${je_cv_lg_page}" != "x" ; then
  LG_PAGE="${je_cv_lg_page}"
fi
if test "x${LG_PAGE}" != "xundefined" ; then
   AC_DEFINE_UNQUOTED([LG_PAGE], [$LG_PAGE])
else
   AC_MSG_ERROR([cannot determine value for LG_PAGE])
fi

AC_ARG_WITH([lg_page_sizes],
  [AS_HELP_STRING([--with-lg-page-sizes=<lg-page-sizes>],
   [Base 2 logs of system page sizes to support])],
  [LG_PAGE_SIZES="$with_lg_page_sizes"], [LG_PAGE_SIZES="$LG_PAGE"])

AC_ARG_WITH([lg_size_class_group],
  [AS_HELP_STRING([--with-lg-size-class-group=<lg-size-class-group>],
   [Base 2 log of size classes per doubling])],
  [LG_SIZE_CLASS_GROUP="$with_lg_size_class_group"],
  [LG_SIZE_CLASS_GROUP="2"])

dnl ============================================================================
dnl jemalloc configuration.
dnl 

AC_ARG_WITH([version],
  [AS_HELP_STRING([--with-version=<major>.<minor>.<bugfix>-<nrev>-g<gid>],
   [Version string])],
  [
    echo "${with_version}" | grep ['^[0-9]\+\.[0-9]\+\.[0-9]\+-[0-9]\+-g[0-9a-f]\+$'] 2>&1 1>/dev/null
    if test $? -ne 0 ; then
      AC_MSG_ERROR([${with_version} does not match <major>.<minor>.<bugfix>-<nrev>-g<gid>])
    fi
    echo "$with_version" > "${objroot}VERSION"
  ], [
    dnl Set VERSION if source directory is inside a git repository.
    if test "x`test ! \"${srcroot}\" && cd \"${srcroot}\"; git rev-parse --is-inside-work-tree 2>/dev/null`" = "xtrue" ; then
      dnl Pattern globs aren't powerful enough to match both single- and
      dnl double-digit version numbers, so iterate over patterns to support up
      dnl to version 99.99.99 without any accidental matches.
      for pattern in ['[0-9].[0-9].[0-9]' '[0-9].[0-9].[0-9][0-9]' \
                     '[0-9].[0-9][0-9].[0-9]' '[0-9].[0-9][0-9].[0-9][0-9]' \
                     '[0-9][0-9].[0-9].[0-9]' '[0-9][0-9].[0-9].[0-9][0-9]' \
                     '[0-9][0-9].[0-9][0-9].[0-9]' \
                     '[0-9][0-9].[0-9][0-9].[0-9][0-9]']; do
        (test ! "${srcroot}" && cd "${srcroot}"; git describe --long --abbrev=40 --match="${pattern}") > "${objroot}VERSION.tmp" 2>/dev/null
        if test $? -eq 0 ; then
          mv "${objroot}VERSION.tmp" "${objroot}VERSION"
          break
        fi
      done
    fi
    rm -f "${objroot}VERSION.tmp"
  ])

if test ! -e "${objroot}VERSION" ; then
  if test ! -e "${srcroot}VERSION" ; then
    AC_MSG_RESULT(
      [Missing VERSION file, and unable to generate it; creating bogus VERSION])
    echo "0.0.0-0-g0000000000000000000000000000000000000000" > "${objroot}VERSION"
  else
    cp ${srcroot}VERSION ${objroot}VERSION
  fi
fi
jemalloc_version=`cat "${objroot}VERSION"`
jemalloc_version_major=`echo ${jemalloc_version} | tr ".g-" " " | awk '{print [$]1}'`
jemalloc_version_minor=`echo ${jemalloc_version} | tr ".g-" " " | awk '{print [$]2}'`
jemalloc_version_bugfix=`echo ${jemalloc_version} | tr ".g-" " " | awk '{print [$]3}'`
jemalloc_version_nrev=`echo ${jemalloc_version} | tr ".g-" " " | awk '{print [$]4}'`
jemalloc_version_gid=`echo ${jemalloc_version} | tr ".g-" " " | awk '{print [$]5}'`
AC_SUBST([jemalloc_version])
AC_SUBST([jemalloc_version_major])
AC_SUBST([jemalloc_version_minor])
AC_SUBST([jemalloc_version_bugfix])
AC_SUBST([jemalloc_version_nrev])
AC_SUBST([jemalloc_version_gid])

dnl ============================================================================
dnl Configure pthreads.

if test "x$abi" != "xpecoff" ; then
  AC_CHECK_HEADERS([pthread.h], , [AC_MSG_ERROR([pthread.h is missing])])
  dnl Some systems may embed pthreads functionality in libc; check for libpthread
  dnl first, but try libc too before failing.
  AC_CHECK_LIB([pthread], [pthread_create], [LIBS="$LIBS -lpthread"],
               [AC_SEARCH_LIBS([pthread_create], , ,
                               AC_MSG_ERROR([libpthread is missing]))])
  JE_COMPILABLE([pthread_atfork(3)], [
#include <pthread.h>
], [
  pthread_atfork((void *)0, (void *)0, (void *)0);
], [je_cv_pthread_atfork])
  if test "x${je_cv_pthread_atfork}" = "xyes" ; then
    AC_DEFINE([JEMALLOC_HAVE_PTHREAD_ATFORK], [ ])
  fi
fi

CPPFLAGS="$CPPFLAGS -D_REENTRANT"

dnl Check whether clock_gettime(2) is in libc or librt.
AC_SEARCH_LIBS([clock_gettime], [rt])

dnl Cray wrapper compiler often adds `-lrt` when using `-static`. Check with
dnl `-dynamic` as well in case a user tries to dynamically link in jemalloc
if test "x$je_cv_cray_prgenv_wrapper" = "xyes" ; then
  if test "$ac_cv_search_clock_gettime" != "-lrt"; then
    SAVED_CFLAGS="${CFLAGS}"

    unset ac_cv_search_clock_gettime
    JE_CFLAGS_APPEND([-dynamic])
    AC_SEARCH_LIBS([clock_gettime], [rt])

    CFLAGS="${SAVED_CFLAGS}"
  fi
fi

dnl check for CLOCK_MONOTONIC_COARSE (Linux-specific).
JE_COMPILABLE([clock_gettime(CLOCK_MONOTONIC_COARSE, ...)], [
#include <time.h>
], [
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
], [je_cv_clock_monotonic_coarse])
if test "x${je_cv_clock_monotonic_coarse}" = "xyes" ; then
  AC_DEFINE([JEMALLOC_HAVE_CLOCK_MONOTONIC_COARSE])
fi

dnl check for CLOCK_MONOTONIC.
JE_COMPILABLE([clock_gettime(CLOCK_MONOTONIC, ...)], [
#include <unistd.h>
#include <time.h>
], [
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
#if !defined(_POSIX_MONOTONIC_CLOCK) || _POSIX_MONOTONIC_CLOCK < 0
#  error _POSIX_MONOTONIC_CLOCK missing/invalid
#endif
], [je_cv_clock_monotonic])
if test "x${je_cv_clock_monotonic}" = "xyes" ; then
  AC_DEFINE([JEMALLOC_HAVE_CLOCK_MONOTONIC])
fi

dnl Check for mach_absolute_time().
JE_COMPILABLE([mach_absolute_time()], [
#include <mach/mach_time.h>
], [
	mach_absolute_time();
], [je_cv_mach_absolute_time])
if test "x${je_cv_mach_absolute_time}" = "xyes" ; then
  AC_DEFINE([JEMALLOC_HAVE_MACH_ABSOLUTE_TIME])
fi

dnl Use syscall(2) (if available) by default.
AC_ARG_ENABLE([syscall],
  [AS_HELP_STRING([--disable-syscall], [Disable use of syscall(2)])],
[if test "x$enable_syscall" = "xno" ; then
  enable_syscall="0"
else
  enable_syscall="1"
fi
],
[enable_syscall="1"]
)
if test "x$enable_syscall" = "x1" ; then
  dnl Check if syscall(2) is usable.  Treat warnings as errors, so that e.g. OS
  dnl X 10.12's deprecation warning prevents use.
  SAVED_CFLAGS="${CFLAGS}"
  JE_CFLAGS_APPEND([-Werror])
  JE_COMPILABLE([syscall(2)], [
#include <sys/syscall.h>
#include <unistd.h>
], [
	syscall(SYS_write, 2, "hello", 5);
],
                [je_cv_syscall])
  CFLAGS="${SAVED_CFLAGS}"
  if test "x$je_cv_syscall" = "xyes" ; then
    AC_DEFINE([JEMALLOC_USE_SYSCALL], [ ])
  fi
fi

dnl Check if the GNU-specific secure_getenv function exists.
AC_CHECK_FUNC([secure_getenv],
              [have_secure_getenv="1"],
              [have_secure_getenv="0"]
             )
if test "x$have_secure_getenv" = "x1" ; then
  AC_DEFINE([JEMALLOC_HAVE_SECURE_GETENV], [ ])
fi

dnl Check if the Solaris/BSD issetugid function exists.
AC_CHECK_FUNC([issetugid],
              [have_issetugid="1"],
              [have_issetugid="0"]
             )
if test "x$have_issetugid" = "x1" ; then
  AC_DEFINE([JEMALLOC_HAVE_ISSETUGID], [ ])
fi

dnl Check whether the BSD-specific _malloc_thread_cleanup() exists.  If so, use
dnl it rather than pthreads TSD cleanup functions to support cleanup during
dnl thread exit, in order to avoid pthreads library recursion during
dnl bootstrapping.
AC_CHECK_FUNC([_malloc_thread_cleanup],
              [have__malloc_thread_cleanup="1"],
              [have__malloc_thread_cleanup="0"]
             )
if test "x$have__malloc_thread_cleanup" = "x1" ; then
  AC_DEFINE([JEMALLOC_MALLOC_THREAD_CLEANUP], [ ])
  force_tls="1"
fi

dnl Check whether the BSD-specific _pthread_mutex_init_calloc_cb() exists.  If
dnl so, mutex initialization causes allocation, and we need to implement this
dnl callback function in order to prevent recursive allocation.
AC_CHECK_FUNC([_pthread_mutex_init_calloc_cb],
              [have__pthread_mutex_init_calloc_cb="1"],
              [have__pthread_mutex_init_calloc_cb="0"]
             )
if test "x$have__pthread_mutex_init_calloc_cb" = "x1" ; then
  AC_DEFINE([JEMALLOC_MUTEX_INIT_CB])
fi

dnl Disable lazy locking by default.
AC_ARG_ENABLE([lazy_lock],
  [AS_HELP_STRING([--enable-lazy-lock],
  [Enable lazy locking (only lock when multi-threaded)])],
[if test "x$enable_lazy_lock" = "xno" ; then
  enable_lazy_lock="0"
else
  enable_lazy_lock="1"
fi
],
[enable_lazy_lock=""]
)
if test "x${enable_lazy_lock}" = "x" ; then
  if test "x${force_lazy_lock}" = "x1" ; then
    AC_MSG_RESULT([Forcing lazy-lock to avoid allocator/threading bootstrap issues])
    enable_lazy_lock="1"
  else
    enable_lazy_lock="0"
  fi
fi
if test "x${enable_lazy_lock}" = "x1" -a "x${abi}" = "xpecoff" ; then
  AC_MSG_RESULT([Forcing no lazy-lock because thread creation monitoring is unimplemented])
  enable_lazy_lock="0"
fi
if test "x$enable_lazy_lock" = "x1" ; then
  if test "x$abi" != "xpecoff" ; then
    AC_CHECK_HEADERS([dlfcn.h], , [AC_MSG_ERROR([dlfcn.h is missing])])
    AC_CHECK_FUNC([dlsym], [],
      [AC_CHECK_LIB([dl], [dlsym], [LIBS="$LIBS -ldl"],
                    [AC_MSG_ERROR([libdl is missing])])
      ])
  fi
  AC_DEFINE([JEMALLOC_LAZY_LOCK], [ ])
fi
AC_SUBST([enable_lazy_lock])

AC_ARG_ENABLE([tls],
  [AS_HELP_STRING([--disable-tls], [Disable thread-local storage (__thread keyword)])],
if test "x$enable_tls" = "xno" ; then
  enable_tls="0"
else
  enable_tls="1"
fi
,
enable_tls=""
)
if test "x${enable_tls}" = "x" ; then
  if test "x${force_tls}" = "x1" ; then
    AC_MSG_RESULT([Forcing TLS to avoid allocator/threading bootstrap issues])
    enable_tls="1"
  elif test "x${force_tls}" = "x0" ; then
    AC_MSG_RESULT([Forcing no TLS to avoid allocator/threading bootstrap issues])
    enable_tls="0"
  else
    enable_tls="1"
  fi
fi
if test "x${enable_tls}" = "x1" ; then
AC_MSG_CHECKING([for TLS])
AC_COMPILE_IFELSE([AC_LANG_PROGRAM(
[[
    __thread int x;
]], [[
    x = 42;

    return 0;
]])],
              AC_MSG_RESULT([yes]),
              AC_MSG_RESULT([no])
              enable_tls="0")
else
  enable_tls="0"
fi
AC_SUBST([enable_tls])
if test "x${enable_tls}" = "x1" ; then
  if test "x${force_tls}" = "x0" ; then
    AC_MSG_WARN([TLS enabled despite being marked unusable on this platform])
  fi
  AC_DEFINE_UNQUOTED([JEMALLOC_TLS], [ ])
elif test "x${force_tls}" = "x1" ; then
  AC_MSG_WARN([TLS disabled despite being marked critical on this platform])
fi

dnl ============================================================================
dnl Check for C11 atomics.

JE_COMPILABLE([C11 atomics], [
#include <stdint.h>
#if (__STDC_VERSION__ >= 201112L) && !defined(__STDC_NO_ATOMICS__)
#include <stdatomic.h>
#else
#error Atomics not available
#endif
], [
    uint64_t *p = (uint64_t *)0;
    uint64_t x = 1;
    volatile atomic_uint_least64_t *a = (volatile atomic_uint_least64_t *)p;
    uint64_t r = atomic_fetch_add(a, x) + x;
    return (r == 0);
], [je_cv_c11atomics])
if test "x${je_cv_c11atomics}" = "xyes" ; then
  AC_DEFINE([JEMALLOC_C11ATOMICS])
fi

dnl ============================================================================
dnl Check for atomic(9) operations as provided on FreeBSD.

JE_COMPILABLE([atomic(9)], [
#include <sys/types.h>
#include <machine/atomic.h>
#include <inttypes.h>
], [
	{
		uint32_t x32 = 0;
		volatile uint32_t *x32p = &x32;
		atomic_fetchadd_32(x32p, 1);
	}
	{
		unsigned long xlong = 0;
		volatile unsigned long *xlongp = &xlong;
		atomic_fetchadd_long(xlongp, 1);
	}
], [je_cv_atomic9])
if test "x${je_cv_atomic9}" = "xyes" ; then
  AC_DEFINE([JEMALLOC_ATOMIC9])
fi

dnl ============================================================================
dnl Check for atomic(3) operations as provided on Darwin.

JE_COMPILABLE([Darwin OSAtomic*()], [
#include <libkern/OSAtomic.h>
#include <inttypes.h>
], [
	{
		int32_t x32 = 0;
		volatile int32_t *x32p = &x32;
		OSAtomicAdd32(1, x32p);
	}
	{
		int64_t x64 = 0;
		volatile int64_t *x64p = &x64;
		OSAtomicAdd64(1, x64p);
	}
], [je_cv_osatomic])
if test "x${je_cv_osatomic}" = "xyes" ; then
  AC_DEFINE([JEMALLOC_OSATOMIC], [ ])
fi

dnl ============================================================================
dnl Check for madvise(2).

JE_COMPILABLE([madvise(2)], [
#include <sys/mman.h>
], [
	madvise((void *)0, 0, 0);
], [je_cv_madvise])
if test "x${je_cv_madvise}" = "xyes" ; then
  AC_DEFINE([JEMALLOC_HAVE_MADVISE], [ ])

  dnl Check for madvise(..., MADV_FREE).
  JE_COMPILABLE([madvise(..., MADV_FREE)], [
#include <sys/mman.h>
], [
	madvise((void *)0, 0, MADV_FREE);
], [je_cv_madv_free])
  if test "x${je_cv_madv_free}" = "xyes" ; then
    AC_DEFINE([JEMALLOC_PURGE_MADVISE_FREE], [ ])
  fi

  dnl Check for madvise(..., MADV_DONTNEED).
  JE_COMPILABLE([madvise(..., MADV_DONTNEED)], [
#include <sys/mman.h>
], [
	madvise((void *)0, 0, MADV_DONTNEED);
], [je_cv_madv_dontneed])
  if test "x${je_cv_madv_dontneed}" = "xyes" ; then
    AC_DEFINE([JEMALLOC_PURGE_MADVISE_DONTNEED], [ ])
  fi

  dnl Check for madvise(..., MADV_[NO]HUGEPAGE).
  JE_COMPILABLE([madvise(..., MADV_[[NO]]HUGEPAGE)], [
#include <sys/mman.h>
], [
	madvise((void *)0, 0, MADV_HUGEPAGE);
	madvise((void *)0, 0, MADV_NOHUGEPAGE);
], [je_cv_thp])
  if test "x${je_cv_thp}" = "xyes" ; then
    AC_DEFINE([JEMALLOC_THP], [ ])
  fi
fi

dnl ============================================================================
dnl Check whether __sync_{add,sub}_and_fetch() are available despite
dnl __GCC_HAVE_SYNC_COMPARE_AND_SWAP_n macros being undefined.

AC_DEFUN([JE_SYNC_COMPARE_AND_SWAP_CHECK],[
  AC_CACHE_CHECK([whether to force $1-bit __sync_{add,sub}_and_fetch()],
               [je_cv_sync_compare_and_swap_$2],
               [AC_LINK_IFELSE([AC_LANG_PROGRAM([
                                                 #include <stdint.h>
                                                ],
                                                [
                                                 #ifndef __GCC_HAVE_SYNC_COMPARE_AND_SWAP_$2
                                                 {
                                                    uint$1_t x$1 = 0;
                                                    __sync_add_and_fetch(&x$1, 42);
                                                    __sync_sub_and_fetch(&x$1, 1);
                                                 }
                                                 #else
                                                 #error __GCC_HAVE_SYNC_COMPARE_AND_SWAP_$2 is defined, no need to force
                                                 #endif
                                                ])],
                               [je_cv_sync_compare_and_swap_$2=yes],
                               [je_cv_sync_compare_and_swap_$2=no])])

  if test "x${je_cv_sync_compare_and_swap_$2}" = "xyes" ; then
    AC_DEFINE([JE_FORCE_SYNC_COMPARE_AND_SWAP_$2], [ ])
  fi
])

if test "x${je_cv_atomic9}" != "xyes" -a "x${je_cv_osatomic}" != "xyes" ; then
  JE_SYNC_COMPARE_AND_SWAP_CHECK(32, 4)
  JE_SYNC_COMPARE_AND_SWAP_CHECK(64, 8)
fi

dnl ============================================================================
dnl Check for __builtin_clz() and __builtin_clzl().

AC_CACHE_CHECK([for __builtin_clz],
               [je_cv_builtin_clz],
               [AC_LINK_IFELSE([AC_LANG_PROGRAM([],
                                                [
                                                {
                                                        unsigned x = 0;
                                                        int y = __builtin_clz(x);
                                                }
                                                {
                                                        unsigned long x = 0;
                                                        int y = __builtin_clzl(x);
                                                }
                                                ])],
                               [je_cv_builtin_clz=yes],
                               [je_cv_builtin_clz=no])])

if test "x${je_cv_builtin_clz}" = "xyes" ; then
  AC_DEFINE([JEMALLOC_HAVE_BUILTIN_CLZ], [ ])
fi

dnl ============================================================================
dnl Check for os_unfair_lock operations as provided on Darwin.

JE_COMPILABLE([Darwin os_unfair_lock_*()], [
#include <os/lock.h>
#include <AvailabilityMacros.h>
], [
	#if MAC_OS_X_VERSION_MIN_REQUIRED < 101200
	#error "os_unfair_lock is not supported"
	#else
	os_unfair_lock lock = OS_UNFAIR_LOCK_INIT;
	os_unfair_lock_lock(&lock);
	os_unfair_lock_unlock(&lock);
	#endif
], [je_cv_os_unfair_lock])
if test "x${je_cv_os_unfair_lock}" = "xyes" ; then
  AC_DEFINE([JEMALLOC_OS_UNFAIR_LOCK], [ ])
fi

dnl ============================================================================
dnl Check for spinlock(3) operations as provided on Darwin.

JE_COMPILABLE([Darwin OSSpin*()], [
#include <libkern/OSAtomic.h>
#include <inttypes.h>
], [
	OSSpinLock lock = 0;
	OSSpinLockLock(&lock);
	OSSpinLockUnlock(&lock);
], [je_cv_osspin])
if test "x${je_cv_osspin}" = "xyes" ; then
  AC_DEFINE([JEMALLOC_OSSPIN], [ ])
fi

dnl ============================================================================
dnl Darwin-related configuration.

AC_ARG_ENABLE([zone-allocator],
  [AS_HELP_STRING([--disable-zone-allocator],
                  [Disable zone allocator for Darwin])],
[if test "x$enable_zone_allocator" = "xno" ; then
  enable_zone_allocator="0"
else
  enable_zone_allocator="1"
fi
],
[if test "x${abi}" = "xmacho"; then
  enable_zone_allocator="1"
fi
]
)
AC_SUBST([enable_zone_allocator])

if test "x${enable_zone_allocator}" = "x1" ; then
  if test "x${abi}" != "xmacho"; then
    AC_MSG_ERROR([--enable-zone-allocator is only supported on Darwin])
  fi
  AC_DEFINE([JEMALLOC_ZONE], [ ])

  dnl The szone version jumped from 3 to 6 between the OS X 10.5.x and 10.6
  dnl releases.  malloc_zone_t and malloc_introspection_t have new fields in
  dnl 10.6, which is the only source-level indication of the change.
  AC_MSG_CHECKING([malloc zone version])
  AC_DEFUN([JE_ZONE_PROGRAM],
    [AC_LANG_PROGRAM(
      [#include <malloc/malloc.h>],
      [static int foo[[sizeof($1) $2 sizeof(void *) * $3 ? 1 : -1]]]
    )])

  AC_COMPILE_IFELSE([JE_ZONE_PROGRAM(malloc_zone_t,==,14)],[JEMALLOC_ZONE_VERSION=3],[
  AC_COMPILE_IFELSE([JE_ZONE_PROGRAM(malloc_zone_t,==,15)],[JEMALLOC_ZONE_VERSION=5],[
  AC_COMPILE_IFELSE([JE_ZONE_PROGRAM(malloc_zone_t,==,16)],[
    AC_COMPILE_IFELSE([JE_ZONE_PROGRAM(malloc_introspection_t,==,9)],[JEMALLOC_ZONE_VERSION=6],[
    AC_COMPILE_IFELSE([JE_ZONE_PROGRAM(malloc_introspection_t,==,13)],[JEMALLOC_ZONE_VERSION=7],[JEMALLOC_ZONE_VERSION=]
  )])],[
  AC_COMPILE_IFELSE([JE_ZONE_PROGRAM(malloc_zone_t,==,17)],[JEMALLOC_ZONE_VERSION=8],[
  AC_COMPILE_IFELSE([JE_ZONE_PROGRAM(malloc_zone_t,>,17)],[JEMALLOC_ZONE_VERSION=9],[JEMALLOC_ZONE_VERSION=]
  )])])])])
  if test "x${JEMALLOC_ZONE_VERSION}" = "x"; then
    AC_MSG_RESULT([unsupported])
    AC_MSG_ERROR([Unsupported malloc zone version])
  fi
  if test "${JEMALLOC_ZONE_VERSION}" = 9; then
    JEMALLOC_ZONE_VERSION=8
    AC_MSG_RESULT([> 8])
  else
    AC_MSG_RESULT([$JEMALLOC_ZONE_VERSION])
  fi
  AC_DEFINE_UNQUOTED(JEMALLOC_ZONE_VERSION, [$JEMALLOC_ZONE_VERSION])
fi

dnl ============================================================================
dnl Check for glibc malloc hooks

JE_COMPILABLE([glibc malloc hook], [
#include <stddef.h>

extern void (* __free_hook)(void *ptr);
extern void *(* __malloc_hook)(size_t size);
extern void *(* __realloc_hook)(void *ptr, size_t size);
], [
  void *ptr = 0L;
  if (__malloc_hook) ptr = __malloc_hook(1);
  if (__realloc_hook) ptr = __realloc_hook(ptr, 2);
  if (__free_hook && ptr) __free_hook(ptr);
], [je_cv_glibc_malloc_hook])
if test "x${je_cv_glibc_malloc_hook}" = "xyes" ; then
  AC_DEFINE([JEMALLOC_GLIBC_MALLOC_HOOK], [ ])
fi

JE_COMPILABLE([glibc memalign hook], [
#include <stddef.h>

extern void *(* __memalign_hook)(size_t alignment, size_t size);
], [
  void *ptr = 0L;
  if (__memalign_hook) ptr = __memalign_hook(16, 7);
], [je_cv_glibc_memalign_hook])
if test "x${je_cv_glibc_memalign_hook}" = "xyes" ; then
  AC_DEFINE([JEMALLOC_GLIBC_MEMALIGN_HOOK], [ ])
fi

JE_COMPILABLE([pthreads adaptive mutexes], [
#include <pthread.h>
], [
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ADAPTIVE_NP);
  pthread_mutexattr_destroy(&attr);
], [je_cv_pthread_mutex_adaptive_np])
if test "x${je_cv_pthread_mutex_adaptive_np}" = "xyes" ; then
  AC_DEFINE([JEMALLOC_HAVE_PTHREAD_MUTEX_ADAPTIVE_NP], [ ])
fi

dnl ============================================================================
dnl Check for typedefs, structures, and compiler characteristics.
AC_HEADER_STDBOOL

dnl ============================================================================
dnl Define commands that generate output files.

AC_CONFIG_COMMANDS([include/jemalloc/internal/private_namespace.h], [
  mkdir -p "${objroot}include/jemalloc/internal"
  "${srcdir}/include/jemalloc/internal/private_namespace.sh" "${srcdir}/include/jemalloc/internal/private_symbols.txt" > "${objroot}include/jemalloc/internal/private_namespace.h"
], [
  srcdir="${srcdir}"
  objroot="${objroot}"
])
AC_CONFIG_COMMANDS([include/jemalloc/internal/private_unnamespace.h], [
  mkdir -p "${objroot}include/jemalloc/internal"
  "${srcdir}/include/jemalloc/internal/private_unnamespace.sh" "${srcdir}/include/jemalloc/internal/private_symbols.txt" > "${objroot}include/jemalloc/internal/private_unnamespace.h"
], [
  srcdir="${srcdir}"
  objroot="${objroot}"
])
AC_CONFIG_COMMANDS([include/jemalloc/internal/public_symbols.txt], [
  f="${objroot}include/jemalloc/internal/public_symbols.txt"
  mkdir -p "${objroot}include/jemalloc/internal"
  cp /dev/null "${f}"
  for nm in `echo ${mangling_map} |tr ',' ' '` ; do
    n=`echo ${nm} |tr ':' ' ' |awk '{print $[]1}'`
    m=`echo ${nm} |tr ':' ' ' |awk '{print $[]2}'`
    echo "${n}:${m}" >> "${f}"
    dnl Remove name from public_syms so that it isn't redefined later.
    public_syms=`for sym in ${public_syms}; do echo "${sym}"; done |grep -v "^${n}\$" |tr '\n' ' '`
  done
  for sym in ${public_syms} ; do
    n="${sym}"
    m="${JEMALLOC_PREFIX}${sym}"
    echo "${n}:${m}" >> "${f}"
  done
], [
  srcdir="${srcdir}"
  objroot="${objroot}"
  mangling_map="${mangling_map}"
  public_syms="${public_syms}"
  JEMALLOC_PREFIX="${JEMALLOC_PREFIX}"
])
AC_CONFIG_COMMANDS([include/jemalloc/internal/public_namespace.h], [
  mkdir -p "${objroot}include/jemalloc/internal"
  "${srcdir}/include/jemalloc/internal/public_namespace.sh" "${objroot}include/jemalloc/internal/public_symbols.txt" > "${objroot}include/jemalloc/internal/public_namespace.h"
], [
  srcdir="${srcdir}"
  objroot="${objroot}"
])
AC_CONFIG_COMMANDS([include/jemalloc/internal/public_unnamespace.h], [
  mkdir -p "${objroot}include/jemalloc/internal"
  "${srcdir}/include/jemalloc/internal/public_unnamespace.sh" "${objroot}include/jemalloc/internal/public_symbols.txt" > "${objroot}include/jemalloc/internal/public_unnamespace.h"
], [
  srcdir="${srcdir}"
  objroot="${objroot}"
])
AC_CONFIG_COMMANDS([include/jemalloc/internal/size_classes.h], [
  mkdir -p "${objroot}include/jemalloc/internal"
  "${SHELL}" "${srcdir}/include/jemalloc/internal/size_classes.sh" "${LG_QUANTA}" ${LG_TINY_MIN} "${LG_PAGE_SIZES}" ${LG_SIZE_CLASS_GROUP} > "${objroot}include/jemalloc/internal/size_classes.h"
], [
  SHELL="${SHELL}"
  srcdir="${srcdir}"
  objroot="${objroot}"
  LG_QUANTA="${LG_QUANTA}"
  LG_TINY_MIN=${LG_TINY_MIN}
  LG_PAGE_SIZES="${LG_PAGE_SIZES}"
  LG_SIZE_CLASS_GROUP=${LG_SIZE_CLASS_GROUP}
])
AC_CONFIG_COMMANDS([include/jemalloc/jemalloc_protos_jet.h], [
  mkdir -p "${objroot}include/jemalloc"
  cat "${srcdir}/include/jemalloc/jemalloc_protos.h.in" | sed -e 's/@je_@/jet_/g' > "${objroot}include/jemalloc/jemalloc_protos_jet.h"
], [
  srcdir="${srcdir}"
  objroot="${objroot}"
])
AC_CONFIG_COMMANDS([include/jemalloc/jemalloc_rename.h], [
  mkdir -p "${objroot}include/jemalloc"
  "${srcdir}/include/jemalloc/jemalloc_rename.sh" "${objroot}include/jemalloc/internal/public_symbols.txt" > "${objroot}include/jemalloc/jemalloc_rename.h"
], [
  srcdir="${srcdir}"
  objroot="${objroot}"
])
AC_CONFIG_COMMANDS([include/jemalloc/jemalloc_mangle.h], [
  mkdir -p "${objroot}include/jemalloc"
  "${srcdir}/include/jemalloc/jemalloc_mangle.sh" "${objroot}include/jemalloc/internal/public_symbols.txt" je_ > "${objroot}include/jemalloc/jemalloc_mangle.h"
], [
  srcdir="${srcdir}"
  objroot="${objroot}"
])
AC_CONFIG_COMMANDS([include/jemalloc/jemalloc_mangle_jet.h], [
  mkdir -p "${objroot}include/jemalloc"
  "${srcdir}/include/jemalloc/jemalloc_mangle.sh" "${objroot}include/jemalloc/internal/public_symbols.txt" jet_ > "${objroot}include/jemalloc/jemalloc_mangle_jet.h"
], [
  srcdir="${srcdir}"
  objroot="${objroot}"
])
AC_CONFIG_COMMANDS([include/jemalloc/jemalloc.h], [
  mkdir -p "${objroot}include/jemalloc"
  "${srcdir}/include/jemalloc/jemalloc.sh" "${objroot}" > "${objroot}include/jemalloc/jemalloc${install_suffix}.h"
], [
  srcdir="${srcdir}"
  objroot="${objroot}"
  install_suffix="${install_suffix}"
])

dnl Process .in files.
AC_SUBST([cfghdrs_in])
AC_SUBST([cfghdrs_out])
AC_CONFIG_HEADERS([$cfghdrs_tup])

dnl ============================================================================
dnl Generate outputs.

AC_CONFIG_FILES([$cfgoutputs_tup config.stamp bin/jemalloc-config bin/jemalloc.sh bin/jeprof])
AC_SUBST([cfgoutputs_in])
AC_SUBST([cfgoutputs_out])
AC_OUTPUT

dnl ============================================================================
dnl Print out the results of configuration.
AC_MSG_RESULT([===============================================================================])
AC_MSG_RESULT([jemalloc version   : ${jemalloc_version}])
AC_MSG_RESULT([library revision   : ${rev}])
AC_MSG_RESULT([])
AC_MSG_RESULT([CONFIG             : ${CONFIG}])
AC_MSG_RESULT([CC                 : ${CC}])
AC_MSG_RESULT([CFLAGS             : ${CFLAGS}])
AC_MSG_RESULT([EXTRA_CFLAGS       : ${EXTRA_CFLAGS}])
AC_MSG_RESULT([CPPFLAGS           : ${CPPFLAGS}])
AC_MSG_RESULT([LDFLAGS            : ${LDFLAGS}])
AC_MSG_RESULT([EXTRA_LDFLAGS      : ${EXTRA_LDFLAGS}])
AC_MSG_RESULT([LIBS               : ${LIBS}])
AC_MSG_RESULT([RPATH_EXTRA        : ${RPATH_EXTRA}])
AC_MSG_RESULT([])
AC_MSG_RESULT([XSLTPROC           : ${XSLTPROC}])
AC_MSG_RESULT([XSLROOT            : ${XSLROOT}])
AC_MSG_RESULT([])
AC_MSG_RESULT([PREFIX             : ${PREFIX}])
AC_MSG_RESULT([BINDIR             : ${BINDIR}])
AC_MSG_RESULT([DATADIR            : ${DATADIR}])
AC_MSG_RESULT([INCLUDEDIR         : ${INCLUDEDIR}])
AC_MSG_RESULT([LIBDIR             : ${LIBDIR}])
AC_MSG_RESULT([MANDIR             : ${MANDIR}])
AC_MSG_RESULT([])
AC_MSG_RESULT([srcroot            : ${srcroot}])
AC_MSG_RESULT([abs_srcroot        : ${abs_srcroot}])
AC_MSG_RESULT([objroot            : ${objroot}])
AC_MSG_RESULT([abs_objroot        : ${abs_objroot}])
AC_MSG_RESULT([])
AC_MSG_RESULT([JEMALLOC_PREFIX    : ${JEMALLOC_PREFIX}])
AC_MSG_RESULT([JEMALLOC_PRIVATE_NAMESPACE])
AC_MSG_RESULT([                   : ${JEMALLOC_PRIVATE_NAMESPACE}])
AC_MSG_RESULT([install_suffix     : ${install_suffix}])
AC_MSG_RESULT([malloc_conf        : ${config_malloc_conf}])
AC_MSG_RESULT([autogen            : ${enable_autogen}])
AC_MSG_RESULT([cc-silence         : ${enable_cc_silence}])
AC_MSG_RESULT([debug              : ${enable_debug}])
AC_MSG_RESULT([code-coverage      : ${enable_code_coverage}])
AC_MSG_RESULT([stats              : ${enable_stats}])
AC_MSG_RESULT([prof               : ${enable_prof}])
AC_MSG_RESULT([prof-libunwind     : ${enable_prof_libunwind}])
AC_MSG_RESULT([prof-libgcc        : ${enable_prof_libgcc}])
AC_MSG_RESULT([prof-gcc           : ${enable_prof_gcc}])
AC_MSG_RESULT([tcache             : ${enable_tcache}])
AC_MSG_RESULT([fill               : ${enable_fill}])
AC_MSG_RESULT([utrace             : ${enable_utrace}])
AC_MSG_RESULT([valgrind           : ${enable_valgrind}])
AC_MSG_RESULT([xmalloc            : ${enable_xmalloc}])
AC_MSG_RESULT([munmap             : ${enable_munmap}])
AC_MSG_RESULT([lazy_lock          : ${enable_lazy_lock}])
AC_MSG_RESULT([tls                : ${enable_tls}])
AC_MSG_RESULT([cache-oblivious    : ${enable_cache_oblivious}])
AC_MSG_RESULT([===============================================================================])
