#!/bin/bash

# The files that need to be properly formatted.  We'll grow this incrementally
# until it includes all the jemalloc source files (as we convert things over),
# and then just replace it with
#    find -name '*.c' -o -name '*.h' -o -name '*.cpp
FILES=(
)

if command -v clang-format &> /dev/null; then
  CLANG_FORMAT="clang-format"
elif command -v clang-format-8 &> /dev/null; then
  CLANG_FORMAT="clang-format-8"
else
  echo "Couldn't find clang-format."
fi

if ! $CLANG_FORMAT -version | grep "version 8\." &> /dev/null; then
  echo "clang-format is the wrong version."
  exit 1
fi

for file in ${FILES[@]}; do
  if ! cmp --silent $file <($CLANG_FORMAT $file) &> /dev/null; then
    echo "Error: $file is not clang-formatted"
    exit 1
  fi
done
