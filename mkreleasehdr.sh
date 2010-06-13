#!/bin/sh
GIT_SHA1=`(git show-ref --head --hash=8 2> /dev/null || echo 00000000) | head -n1`
GIT_DIRTY=`git diff 2> /dev/null | wc -l`
test -f release.h || touch release.h
(cat release.h | grep SHA1 | grep $GIT_SHA1) && \
(cat release.h | grep DIRTY | grep $GIT_DIRTY) && exit 0 # Already uptodate
echo "#define REDIS_GIT_SHA1 \"$GIT_SHA1\"" > release.h
echo "#define REDIS_GIT_DIRTY \"$GIT_DIRTY\"" >> release.h
touch release.c # Force recompile of release.c
