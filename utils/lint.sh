#!/bin/bash

# Exit on any command failure
set -e

branch=$1

target="/tmp/redis-$RANDOM"

# Sanity check our random dir doesn't already exist
while [[ -d "$target" ]]; do
    target="/tmp/redis-$RANDOM"
done

here=`dirname $0`

git clone "$here/.." "$target"

pushd "$target"

if [[ ! -z $branch ]]; then
    git checkout $branch
else
    git checkout unstable
fi

# We should make with clang and gcc since they
# output different warnings, *but* we can't
# know which compilers the target system will
# have.  clang + gcc?  clang + gcc-4.9? ...
make -j REDIS_CFLAGS="-Werror"

./runtest

./runtest-cluster

./runtest-sentinel

popd

rm -rf "$target"

echo "Passed building and running all tests!"
