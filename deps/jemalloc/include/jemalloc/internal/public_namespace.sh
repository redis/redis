#!/bin/sh

for nm in `cat $1` ; do
  n=`echo ${nm} |tr ':' ' ' |awk '{print $1}'`
  echo "#define	je_${n} JEMALLOC_N(${n})"
done
