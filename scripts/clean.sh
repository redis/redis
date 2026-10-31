#!/bin/sh
# clean.sh — clean Redis (src/) + selected modules.
#
# Usage:  scripts/clean.sh [<name> ...|all|.|'*'|redis|none]
#
# Same selector semantics as scripts/build.sh:
#   (no args) | all | . | '*'   clean Redis + every cloned module
#   redis | none                 clean Redis only
#   <name> [<name> ...]          clean Redis + the listed modules
#
# Env vars set on the `make clean` invocation (e.g. `make clean DEPS=1`)
# flow through to per-module `make -C modules/<name> -f common.mk clean`
# automatically via the shell's environment — no need to list them here.
#
# Best-effort: a failure on one module doesn't block the rest. Exits 0
# even on partial failure so a clean attempt is never destructive.
#
# Backs `make clean`, so like build.sh it must stay POSIX-sh clean —
# no bashisms (see scripts/build.sh header).

set -eu

SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)" || exit 1
. "$SCRIPT_DIR/lib/manifest.sh"
cd "$REPO_ROOT"

MAKE_BIN="${MAKE:-make}"

cloned="$(cloned_modules)"
modules="$(resolve_modules "$*" "$cloned" "redis none")"

echo "==> Cleaning main Redis (src/)"
"$MAKE_BIN" -C src clean || true

if [ -z "$modules" ]; then
  exit 0
fi

echo
echo "==> Cleaning modules: $modules"
for name in $modules; do
  if [ -d "modules/$name/src" ]; then
    echo
    echo "==> [clean] $name (modules/$name)"
    "$MAKE_BIN" -C "modules/$name" -f "$REPO_ROOT/modules/common.mk" clean || true
  fi
done

echo
echo "==> Clean complete."
