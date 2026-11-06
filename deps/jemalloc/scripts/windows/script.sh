#!/bin/bash

set -e

if [[ "$TRAVIS_OS_NAME" != "windows" ]]; then
    echo "Incorrect \$TRAVIS_OS_NAME: expected windows, got $TRAVIS_OS_NAME"
    exit 1
fi

$build_env mingw32-make -k check
