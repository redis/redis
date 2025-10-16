#!/bin/bash

# Exit immediately if any command fails.
# https://stackoverflow.com/a/2871034
set -e -u -x


# Normal
./xxhsum ./Makefile > ./.test.xxh
./xxhsum --check ./.test.xxh


# Missing, expect error
# (1) Create checksum file.
# (2) Remove one of them.
# (3) --check it
# (4) Expect NG (missing file)
cp Makefile .test.makefile
./xxhsum ./.test.makefile > ./.test.xxh
rm ./.test.makefile
! ./xxhsum --check ./.test.xxh  # Put '!' for expecting error


# Missing, --ignore-missing
# (1) Create checksum file.
# (2) Remove one of them.
# (3) --check it with --ignore-missing.
# (4) Expect OK

cp Makefile .test.makefile
./xxhsum Makefile ./.test.makefile > ./.test.xxh
rm ./.test.makefile
./xxhsum --check --ignore-missing ./.test.xxh


# Missing, --ignore-missing, expect error
# (1) Create checksum file.
# (2) Remove all of them.
# (3) --check it with --ignore-missing.
# (4) Expect NG (no file was verified).

cp Makefile .test.makefile
./xxhsum ./.test.makefile > ./.test.xxh
rm ./.test.makefile
! ./xxhsum --check --ignore-missing ./.test.xxh  # Put '!' for expecting error


# Cleanup
( rm ./.test.* ) || true

echo OK
