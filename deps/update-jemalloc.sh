#!/bin/bash
VER=$1
URL="http://www.canonware.com/download/jemalloc/jemalloc-${VER}.tar.bz2"
echo "Downloading $URL"
curl $URL > /tmp/jemalloc.tar.bz2
tar xvjf /tmp/jemalloc.tar.bz2
rm -rf jemalloc
mv jemalloc-${VER} jemalloc

## Change LG_QUANTUM size from 4 to 3 for  AMD64 and I386.
## https://github.com/antirez/redis/commit/6b836b6b4148a3623e35807e998097865b9ebb3a
echo "Update LG_QUANTUM values ..."
for i in _M_IX86 _M_X64
do
    sed -i "/$i/!b;n;c#    define LG_QUANTUM		3" jemalloc/include/jemalloc/internal/jemalloc_internal.h.in
done

echo "Use git status, add all files and commit changes."