#!/bin/sh
GIT_SHA1=`(git show-ref --head --hash=8 2> /dev/null || echo 00000000) | head -n1`
GIT_DIRTY=`git diff --no-ext-diff 2> /dev/null | wc -l`
BUILD_ID=`uname -n`"-"`date +%s`
if [ -n "$SOURCE_DATE_EPOCH" ]; then
  BUILD_ID=$(date -u -d "@$SOURCE_DATE_EPOCH" +%s 2>/dev/null || date -u -r "$SOURCE_DATE_EPOCH" +%s 2>/dev/null || date -u +%s)
fi

test -f release.h || touch release.h
(cat release.h | grep SHA1 | grep $GIT_SHA1) && \
(cat release.h | grep DIRTY | grep $GIT_DIRTY) && exit 0 # Already up-to-date
echo "#define REDIS_GIT_SHA1 \"$GIT_SHA1\"" > release.h
echo "#define REDIS_GIT_DIRTY \"$GIT_DIRTY\"" >> release.h
echo "#define REDIS_BUILD_ID \"$BUILD_ID\"" >> release.h
touch release.c # Force recompile of release.c

ROCKSDB_VERSION_MAJOR=$(grep '#define ROCKSDB_MAJOR' ../deps/rocksdb/include/rocksdb/version.h | awk '{print $3}')
ROCKSDB_VERSION_MINOR=$(grep '#define ROCKSDB_MINOR' ../deps/rocksdb/include/rocksdb/version.h | awk '{print $3}')
ROCKSDB_VERSION_PATCH=$(grep '#define ROCKSDB_PATCH' ../deps/rocksdb/include/rocksdb/version.h | awk '{print $3}')
ROCKSDB_VERSION="$ROCKSDB_VERSION_MAJOR.$ROCKSDB_VERSION_MINOR.$ROCKSDB_VERSION_PATCH"
echo "#define ROCKSDB_VERSION \"$ROCKSDB_VERSION\"" >> release.h
touch release.c # Force recompile of release.c
