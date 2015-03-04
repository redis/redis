#!/bin/sh

public_symbols_txt=$1
symbol_prefix=$2

cat <<EOF
/*
 * By default application code must explicitly refer to mangled symbol names,
 * so that it is possible to use jemalloc in conjunction with another allocator
 * in the same application.  Define JEMALLOC_MANGLE in order to cause automatic
 * name mangling that matches the API prefixing that happened as a result of
 * --with-mangling and/or --with-jemalloc-prefix configuration settings.
 */
#ifdef JEMALLOC_MANGLE
#  ifndef JEMALLOC_NO_DEMANGLE
#    define JEMALLOC_NO_DEMANGLE
#  endif
EOF

for nm in `cat ${public_symbols_txt}` ; do
  n=`echo ${nm} |tr ':' ' ' |awk '{print $1}'`
  echo "#  define ${n} ${symbol_prefix}${n}"
done

cat <<EOF
#endif

/*
 * The ${symbol_prefix}* macros can be used as stable alternative names for the
 * public jemalloc API if JEMALLOC_NO_DEMANGLE is defined.  This is primarily
 * meant for use in jemalloc itself, but it can be used by application code to
 * provide isolation from the name mangling specified via --with-mangling
 * and/or --with-jemalloc-prefix.
 */
#ifndef JEMALLOC_NO_DEMANGLE
EOF

for nm in `cat ${public_symbols_txt}` ; do
  n=`echo ${nm} |tr ':' ' ' |awk '{print $1}'`
  echo "#  undef ${symbol_prefix}${n}"
done

cat <<EOF
#endif
EOF
