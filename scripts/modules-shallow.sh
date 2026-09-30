#!/bin/sh
# modules-shallow.sh — re-clone given modules with --depth 1.
#
# Usage:  scripts/modules-shallow.sh <name> [<name> ...]
#         scripts/modules-shallow.sh all | . | '*'
#
# Removes existing modules/<name>/src and delegates to modules-update.sh
# with MODULES_UPDATE_SHALLOW=1 so the clone path stays single-source-of-truth.

set -eu

SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)" || exit 1
. "$SCRIPT_DIR/lib/manifest.sh"
cd "$REPO_ROOT"

cloned="$(cloned_modules)"

if [ "$#" -eq 0 ]; then
  echo "Usage: make modules-shallow <name> [<name> ...]"
  echo "       make modules-shallow all   # or '.' or '*' (quote the star)"
  echo "Cloned modules: $cloned"
  exit 1
fi

if [ -z "$cloned" ]; then
  echo "ERROR: no cloned modules under modules/*/src"
  echo "       run 'make modules-update all' first"
  exit 1
fi

# resolve_modules() rejects mixing all/./'*' with explicit names (unlike the
# old hand-rolled loop here, which silently expanded to every cloned module
# the moment any one token matched a wildcard — dropping/ignoring whatever
# else was on the command line before the destructive rm -rf below).
requested="$(resolve_modules "$*" "$cloned")"

for name in $requested; do
  echo "==> Removing existing clone modules/$name/src to re-clone shallow"
  rm -rf "modules/$name/src"
done

echo
MODULES_UPDATE_SHALLOW=1 exec "$SCRIPT_DIR/modules-update.sh" $requested
