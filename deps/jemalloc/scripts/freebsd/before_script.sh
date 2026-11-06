#!/bin/tcsh

autoconf
# We don't perfectly track freebsd stdlib.h definitions.  This is fine when
# we count as a system header, but breaks otherwise, like during these
# tests.
./configure --with-jemalloc-prefix=ci_ ${COMPILER_FLAGS:+ CC="$CC $COMPILER_FLAGS" CXX="$CXX $COMPILER_FLAGS"} $CONFIGURE_FLAGS
JE_NCPUS=`sysctl -n kern.smp.cpus`
gmake -j${JE_NCPUS}
gmake -j${JE_NCPUS} tests
