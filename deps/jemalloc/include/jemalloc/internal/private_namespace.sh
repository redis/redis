#!/bin/sh

for symbol in `cat "$@"` ; do
  echo "#define ${symbol} JEMALLOC_N(${symbol})"
done
