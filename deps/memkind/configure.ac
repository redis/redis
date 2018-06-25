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

AC_PREREQ([2.64])
AC_INIT([memkind],m4_esyscmd([tr -d '\n' < VERSION]))

AC_CONFIG_HEADERS([config.h])
AC_CONFIG_SRCDIR([memkind.spec.mk])

AM_INIT_AUTOMAKE([-Wall -Werror foreign 1.11 silent-rules subdir-objects parallel-tests tar-pax])
AM_SILENT_RULES([yes])

# Checks for programs and libraries.
AM_PROG_AR
AC_PROG_CXX
AC_PROG_CC
AC_OPENMP
AC_CHECK_LIB(numa, numa_available, [], [AC_MSG_ERROR([libnuma is required dependency])])
AX_PTHREAD([LIBS="$PTHREAD_LIBS $LIBS" CFLAGS="$CFLAGS $PTHREAD_CFLAGS" CC="$PTHREAD_CC"],
           [AC_MSG_ERROR([pthreads are required dependency])])

AM_PROG_CC_C_O

#============================tls===============================================
# Check for tls_model attribute support
SAVED_CFLAGS="${CFLAGS}"
CFLAGS="$CFLAGS -Werror"
AC_MSG_CHECKING([for tls_model attribute support])
AC_COMPILE_IFELSE([AC_LANG_PROGRAM(
[[
    static __thread int __attribute__((tls_model("local-dynamic"))) x;
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
  [AS_HELP_STRING([--enable-decorators], [Enable decorators])],
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

#============================arena_limit=======================================
arena_limit=256;
AC_ARG_VAR(ARENA_LIMIT,
  [Upper bound for number of arenas per kind, if set to 0 then no limit]
)
if test "$ARENA_LIMIT" != "" ; then
  arena_limit=$ARENA_LIMIT;
fi

AC_DEFINE_UNQUOTED([ARENA_LIMIT_PER_KIND], $arena_limit, [Upper bound for number of arenas per kind])

#============================cxx11=============================================

AX_CXX_COMPILE_STDCXX_11([noext], [optional])
AM_CONDITIONAL([HAVE_CXX11], [test "x$HAVE_CXX11" = x1])

LT_PREREQ([2.2])
LT_INIT

AC_CONFIG_FILES([Makefile])

AC_OUTPUT

