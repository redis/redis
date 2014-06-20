#!/bin/sh

for symbol in `cat $1` ; do
  echo "#define	${symbol} JEMALLOC_N(${symbol})"
done
