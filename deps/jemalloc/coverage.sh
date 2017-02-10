#!/bin/sh

set -e

objdir=$1
suffix=$2
shift 2
objs=$@

gcov -b -p -f -o "${objdir}" ${objs}

# Move gcov outputs so that subsequent gcov invocations won't clobber results
# for the same sources with different compilation flags.
for f in `find . -maxdepth 1 -type f -name '*.gcov'` ; do
  mv "${f}" "${f}.${suffix}"
done
