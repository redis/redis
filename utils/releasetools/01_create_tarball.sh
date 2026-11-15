#!/bin/sh
if [ $# != "1" ]
then
    echo "Usage: ./utils/releasetools/01_create_tarball.sh <version_tag>"
    exit 1
fi

# Resolve relative to this script's own location, not the caller's cwd — the
# old version (a plain `git archive`) worked from any directory via git's own
# upward .git search; this shim otherwise only works when invoked from the
# repo root.
cd -- "$(dirname -- "$0")/../.." || exit 1

if [ ! -x scripts/tarball.sh ]; then
    echo "ERROR: scripts/tarball.sh is missing or not executable (run from repo root)." >&2
    exit 1
fi
TAG="$1" exec scripts/tarball.sh
