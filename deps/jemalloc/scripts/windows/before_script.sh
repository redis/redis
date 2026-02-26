#!/bin/bash

set -e

if [[ "$TRAVIS_OS_NAME" != "windows" ]]; then
    echo "Incorrect \$TRAVIS_OS_NAME: expected windows, got $TRAVIS_OS_NAME"
    exit 1
fi

$build_env autoconf
$build_env ./configure $CONFIGURE_FLAGS
# mingw32-make simply means "make", unrelated to mingw32 vs mingw64.
# Simply disregard the prefix and treat is as "make".
$build_env mingw32-make -j3
# At the moment, it's impossible to make tests in parallel,
# seemingly due to concurrent writes to '.pdb' file. I don't know why
# that happens, because we explicitly supply '/Fs' to the compiler.
# Until we figure out how to fix it, we should build tests sequentially
# on Windows.
$build_env mingw32-make tests
