#!/bin/sh

public_symbols_txt=$1

cat <<EOF
/*
 * Name mangling for public symbols is controlled by --with-mangling and
 * --with-jemalloc-prefix.  With default settings the je_ prefix is stripped by
 * these macro definitions.
 */
#ifndef JEMALLOC_NO_RENAME
EOF

for nm in `cat ${public_symbols_txt}` ; do
  n=`echo ${nm} |tr ':' ' ' |awk '{print $1}'`
  m=`echo ${nm} |tr ':' ' ' |awk '{print $2}'`
  echo "#  define je_${n} ${m}"
done

cat <<EOF
#endif
EOF
