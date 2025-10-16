#!/bin/bash

# Exit immediately if any command fails.
# https://stackoverflow.com/a/2871034
set -euxo pipefail

echo filename-escape-test-string > $'filename-escape-foo\nbar'

./xxhsum $'filename-escape-foo\nbar' | tee filename-escape-xxh64.txt
cat filename-escape-xxh64.txt
./xxhsum -c filename-escape-xxh64.txt
hexdump -C filename-escape-xxh64.txt

./xxhsum --tag $'filename-escape-foo\nbar' | tee filename-escape-xxh64-tag.txt
cat filename-escape-xxh64-tag.txt
./xxhsum -c filename-escape-xxh64-tag.txt
hexdump -C filename-escape-xxh64-tag.txt

rm filename-escape-xxh64-tag.txt
rm filename-escape-xxh64.txt
rm $'filename-escape-foo\nbar'
