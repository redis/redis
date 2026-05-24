#!/usr/bin/env bash
# modules-shallow.sh — re-clone given modules with --depth 1.
#
# Usage:  scripts/modules-shallow.sh <name> [<name> ...]
#         scripts/modules-shallow.sh all | . | '*'
#
# Removes existing modules/<name>/src and delegates to modules-update.sh
# with MODULES_UPDATE_SHALLOW=1 so the clone path stays single-source-of-truth.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)" || exit 1
. "$SCRIPT_DIR/lib/manifest.sh"
cd "$REPO_ROOT"

available="$(manifest_modules | xargs)"
cloned=""
for name in $available; do
  [ -d "modules/$name/src/.git" ] && cloned="$cloned $name"
done
cloned="$(echo "$cloned" | xargs)"

requested="$*"
if [ -z "$requested" ]; then
  echo "Usage: make modules-shallow <name> [<name> ...]"
  echo "       make modules-shallow all   # or '.' or '*' (quote the star)"
  echo "Cloned modules: $cloned"
  exit 1
fi

for r in $requested; do
  case "$r" in all|.|'*') requested="$cloned"; break ;; esac
done

if [ -z "$cloned" ]; then
  echo "ERROR: no cloned modules under modules/*/src"
  echo "       run 'make modules-update all' first"
  exit 1
fi

for name in $requested; do
  case " $cloned " in *" $name "*) ;; *)
    echo "ERROR: module '$name' is not cloned at modules/$name/src"
    echo "Cloned modules: $cloned"
    exit 1 ;;
  esac
done

for name in $requested; do
  echo "==> Removing existing clone modules/$name/src to re-clone shallow"
  rm -rf "modules/$name/src"
done

echo
MODULES_UPDATE_SHALLOW=1 exec "$SCRIPT_DIR/modules-update.sh" $requested
