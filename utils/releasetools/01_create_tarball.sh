#!/bin/sh
if [ $# != "1" ]
then
    echo "Usage: ./utils/releasetools/01_create_tarball.sh <version_tag>"
    exit 1
fi

if [ ! -x scripts/tarball.sh ]; then
    echo "ERROR: scripts/tarball.sh is missing or not executable (run from repo root)." >&2
    exit 1
fi
TAG="$1" exec scripts/tarball.sh
