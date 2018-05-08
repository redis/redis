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

#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.62])
AC_INIT([memkind],m4_esyscmd([tr -d '\n' < VERSION]),[krzysztof.kulakowski@intel.com])
AM_INIT_AUTOMAKE([-Wall -Werror foreign subdir-objects tar-pax])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_HEADERS([config_tls.h])
AC_CONFIG_MACRO_DIR([m4])

memkind_abi_version=0:1:0
AC_SUBST(memkind_abi_version)

LT_PREREQ([2.2.6])

# all modifications to CFLAGS/CXXFLAGS shall be done *after* this line
AC_PROG_CC

AC_ARG_WITH([numa], [AS_HELP_STRING([--with-numa=PATH],
            [specify prefix directory for installed numa package.])])
if test "x$with_numa" != x; then
  CPPFLAGS="$CPPFLAGS -I$with_numa/include"
  LD_LIBRARY_PATH="$with_numa/lib:$LD_LIBRARY_PATH"
  LDFLAGS="$LDFLAGS -L$with_numa/lib"
fi
AC_ARG_WITH([numa-include], [AS_HELP_STRING([--with-numa-include=PATH],
            [specify directory for the installed numa include files.])])
if test "x$with_numa_include" != x; then
  CPPFLAGS="$CPPFLAGS -I$with_numa_include"
fi
AC_ARG_WITH([numa-lib], [AS_HELP_STRING([--with-numa-lib=PATH],
            [specify directory for installed numa library])])
if test "x$with_numa_lib" != x; then
  LD_LIBRARY_PATH="$with_numa_lib:$LD_LIBRARY_PATH"
  LDFLAGS="$LDFLAGS -L$with_numa_lib"
fi

#============================tls===============================================
# Check for tls_model attribute support
SAVED_CFLAGS="${CFLAGS}"
CFLAGS="$CFLAGS -Werror"
AC_MSG_CHECKING([for tls_model attribute support])
AC_COMPILE_IFELSE([AC_LANG_PROGRAM(
[[
    static __thread int __attribute__((tls_model("initial-exec"))) x;
]],
[[
    x = 1234;
]])],
    [AC_MSG_RESULT([yes])
     tls_model="1"],
    [AC_MSG_RESULT([no])
     tls_model="0"])

CFLAGS="${SAVED_CFLAGS}"
if test "x${tls_model}" = "x1" ; then
  AC_DEFINE([MEMKIND_TLS_MODEL],
            [__attribute__((tls_model("initial-exec")))], [TLS model attribute])
else
  AC_DEFINE([MEMKIND_TLS_MODEL], [ ], [TLS model attribute])
fi

AC_ARG_ENABLE([tls],
  [AS_HELP_STRING([--enable-tls], [Enable thread-local storage (__thread keyword)])],
[if test "x$enable_tls" = "xyes" ; then
  enable_tls="1"
else
  enable_tls="0"
fi
],
[enable_tls="0"]
)
if test "x${enable_tls}" = "x1" ; then
AC_MSG_CHECKING([for TLS support])
AC_COMPILE_IFELSE([AC_LANG_PROGRAM(
[[
    __thread int x;
]], [[
    x = 1234;
]])],
    AC_MSG_RESULT([yes]),
    AC_MSG_RESULT([no])
    enable_tls="0")
fi
if test "x${enable_tls}" = "x1" ; then
  AC_DEFINE([MEMKIND_TLS], [ ], [Enables TLS usage for mapping arenas to threads])
fi
AC_SUBST([enable_tls])

#============================decorators========================================
AC_ARG_ENABLE([decorators],
  [AS_HELP_STRING([--enable-decorators], [Enable decorators 111])],
[if test "x$enable_decorators" = "xyes" ; then
  enable_decorators="1"
else
  enable_decorators="0"
fi
],
[enable_decorators="0"]
)
if test "x${enable_decorators}" = "x1" ; then
  AC_DEFINE([MEMKIND_DECORATION_ENABLED], [ ], [Enables decorators])
fi
AC_SUBST([enable_decorators])

#============================arena_limit=======================================
arena_limit=256;
AC_ARG_VAR(ARENA_LIMIT,
  [Upper bound for number of arenas per kind, if set to 0 then no limit]
)
if test "$ARENA_LIMIT" != "" ; then
  arena_limit=$ARENA_LIMIT;
fi

AC_DEFINE_UNQUOTED([ARENA_LIMIT_PER_KIND], $arena_limit, [Upper bound for number of arenas per kind])

#============================debug=============================================
AC_ARG_ENABLE([debug],
  [AS_HELP_STRING([--enable-debug], [Build debugging code and compile with -O0 -g])],
[if test "x$enable_debug" = "xno" ; then
  enable_debug="0"
else
  enable_debug="1"
fi
],
[enable_debug="0"]
)
if test "x$enable_debug" = "x1" ; then
  AC_DEFINE([MEMKIND_DEBUG], [ ], [Enables code for debugging])
  CFLAGS="$CFLAGS -O0 -g"
  CXXFLAGS="$CXXFLAGS -O0 -g"
fi
AC_SUBST([enable_debug])

#============================secure_flags======================================
AC_ARG_ENABLE([secure],
  [AS_HELP_STRING([--enable-secure], [Build library with security enchantments])],
[if test "x$enable_secure" = "xno" ; then
  enable_secure="0"
else
  enable_secure="1"
fi
],
[enable_secure="1"]
)
if test "x$enable_secure" = "x1" ; then
  CFLAGS="$CFLAGS -fstack-protector"
  LDFLAGS="$LDFLAGS -Wl,-z,relro,-z,now"

  if test "$CFLAGS" != "${CFLAGS%-O0*}" ; then # if CFLAGS contains -O0
      echo "WARNING: Could not apply FORTIFY_SOURCE=2 due to lack of optimization (-O0)"
  else
      CFLAGS="$CFLAGS -D_FORTIFY_SOURCE=2" #FORTITFY_SOURCE does not work with -O0 (ex. if enable_debug=1)
  fi
fi

AC_SUBST([enable_secure])

#============================cxx11=============================================
AC_ARG_ENABLE([cxx11],
  [AS_HELP_STRING([--enable-cxx11], [Enable tests and examples that use C++11 features])],
[if test "x$enable_cxx11" = "xno" ; then
  enable_cxx11="0"
else
  enable_cxx11="1"
fi
],
[enable_cxx11="0"]
)
if test "x$enable_cxx11" = "x1" ; then
  AC_DEFINE([MEMKIND_CXX11], [ ], [Enables C++11 features in tests and examples])
fi
AC_SUBST([enable_cxx11])
AM_CONDITIONAL([ENABLE_CXX11], [test "x$enable_cxx11" = "x1"])

# Checks for programs.
m4_ifdef([AM_PROG_AR], [AM_PROG_AR])
AC_PROG_CXX
AC_PROG_AWK
AC_PROG_CC
AC_PROG_CPP
AC_PROG_INSTALL
AC_PROG_LN_S
AC_PROG_MAKE_SET
LT_INIT
AM_PROG_CC_C_O

# Below macro is setting OPENMP_CFLAGS, but it is not working correctly for CXXFLAGS
# (even with lang pop/push trick). Current W/A is to use CFLAGS also for C++.
AC_OPENMP

if test "x$OPENMP_CFLAGS" = "x" ; then
  echo "WARNING: Could not locate OpenMP support. This warning can also occur if compiler already supports OpenMP"
fi

AC_CHECK_HEADER([omp.h], [], [
    echo "omp.h is required for this library"
    exit -1])
AC_CHECK_LIB([numa], [numa_available], [], [
    echo "libnuma is required for this library"
    exit -1])
AC_CHECK_HEADER([numa.h], [], [
    echo "numa.h is required for this library"
    exit -1])
AC_CHECK_LIB([pthread], [pthread_mutex_init], [], [
    echo "pthread is required for this library"
    exit -1])
AC_CHECK_HEADER([pthread.h], [], [
    echo "pthread.h is required for this library"
    exit -1])

# Checks for header files.
AC_CHECK_HEADERS([fcntl.h float.h limits.h stdint.h stdlib.h string.h sys/time.h unistd.h utmpx.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_C_INLINE
AC_TYPE_SIZE_T
AC_TYPE_SSIZE_T

# Checks for library functions.
AC_FUNC_MALLOC
AC_FUNC_MMAP
AC_FUNC_REALLOC
AC_CHECK_FUNCS([gettimeofday memset mkdir munmap setenv strtol strtoul])

AC_CONFIG_FILES([Makefile])
AC_OUTPUT

# ============================================================================
# Print out the results of configuration.
AC_MSG_RESULT([===============================================================================])
AC_MSG_RESULT([memkind version    : ${VERSION}])
AC_MSG_RESULT([])
AC_MSG_RESULT([CC                 : ${CC}])
AC_MSG_RESULT([CPPFLAGS           : ${CPPFLAGS}])
AC_MSG_RESULT([CFLAGS             : ${CFLAGS}])
AC_MSG_RESULT([CXXFLAGS           : ${CXXFLAGS}])
AC_MSG_RESULT([OPENMP_CFLAGS      : ${OPENMP_CFLAGS}])
AC_MSG_RESULT([LDFLAGS            : ${LDFLAGS}])
AC_MSG_RESULT([LD_LIBRARY_PATH    : ${LD_LIBRARY_PATH}])
AC_MSG_RESULT([LIBS               : ${LIBS}])
AC_MSG_RESULT([])
AC_MSG_RESULT([prefix             : ${prefix}])
AC_MSG_RESULT([exec_prefix        : ${exec_prefix}])
AC_MSG_RESULT([sbindir            : ${sbindir}])
AC_MSG_RESULT([includedir         : ${includedir}])
AC_MSG_RESULT([libdir             : ${libdir}])
AC_MSG_RESULT([datarootdir        : ${datarootdir}])
AC_MSG_RESULT([datadir            : ${datadir}])
AC_MSG_RESULT([mandir             : ${mandir}])
AC_MSG_RESULT([])
AC_MSG_RESULT([arena_limit        : ${arena_limit}])
AC_MSG_RESULT([debug              : ${enable_debug}])
AC_MSG_RESULT([tls                : ${enable_tls}])
AC_MSG_RESULT([decorators         : ${enable_decorators}])
AC_MSG_RESULT([secure             : ${enable_secure}])
AC_MSG_RESULT([===============================================================================])
