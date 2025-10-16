#!/bin/bash

# Exit immediately if any command fails.
# https://stackoverflow.com/a/2871034
set -euxo


# Default
./xxhsum ./Makefile > ./.test.xxh
echo '# Test comment line' | cat - ./.test.xxh > temp && mv temp ./.test.xxh
./xxhsum --check ./.test.xxh

# XXH32
./xxhsum -H32 ./Makefile > ./.test.xxh32
echo '# Test comment line' | cat - ./.test.xxh32 > temp && mv temp ./.test.xxh32
./xxhsum --check ./.test.xxh32

# XXH64
./xxhsum -H64 ./Makefile > ./.test.xxh64
echo '# Test comment line' | cat - ./.test.xxh64 > temp && mv temp ./.test.xxh64
./xxhsum --check ./.test.xxh64

# XXH128
./xxhsum -H128 ./Makefile > ./.test.xxh128
echo '# Test comment line' | cat - ./.test.xxh128 > temp && mv temp ./.test.xxh128
./xxhsum --check ./.test.xxh128


rm ./.test.xxh
rm ./.test.xxh32
rm ./.test.xxh64
rm ./.test.xxh128
