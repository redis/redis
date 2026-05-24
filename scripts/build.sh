#!/usr/bin/env bash
# build.sh — build Redis (src/) + selected modules.
#
# Usage:  scripts/build.sh [<name> ...|all|.|'*'|redis|none]
#
# Tokens:
#   (no args) | all | . | '*'   build Redis + every cloned module
#   redis | none                 build Redis only
#   <name> [<name> ...]          build Redis + the listed modules
#
# Each selected module is built via `make -C modules/<name>` with
# RM_INCLUDE_DIR/RS_INCLUDE_DIR/REDIS_SERVER pointing at our just-built tree.
# Failures are collected and reported at the end.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)" || exit 1
. "$SCRIPT_DIR/lib/manifest.sh"
cd "$REPO_ROOT"

MAKE_BIN="${MAKE:-make}"

cloned="$(cloned_modules)"
modules="$(resolve_modules "$*" "$cloned" "redis none")"

echo "==> Building main Redis (src/)"
if ! "$MAKE_BIN" -C src all; then
  echo
  echo "ERROR: Redis core build failed (make -C src all)." >&2
  echo "       Fix the failure above before module builds will run." >&2
  exit 1
fi

failed=""
if [ -z "$modules" ]; then
  echo
  if [ -z "$cloned" ]; then
    echo "==> No cloned modules under modules/*/src, Redis-only build"
  else
    echo "==> Module builds skipped by request"
  fi
else
  echo
  echo "==> Building modules against $PWD/src (RM_INCLUDE_DIR) and $PWD/src/redis-server:"
  echo "   $modules"
  for name in $modules; do
    echo
    echo "==> [module] $name (modules/$name)"
    mkdir -p "modules/$name"
    if ! "$MAKE_BIN" -C "modules/$name" -f "$REPO_ROOT/modules/common.mk" \
        RM_INCLUDE_DIR="$REPO_ROOT/src" \
        RS_INCLUDE_DIR="$REPO_ROOT/src" \
        REDIS_SERVER="$REPO_ROOT/src/redis-server"; then
      failed="$failed $name"
      echo "==> [module] $name: FAILED (continuing with remaining modules)"
    fi
  done
  if [ -n "$failed" ]; then
    echo
    echo "==> WARNING: The following module(s) failed to build:$failed"
  fi
fi

echo
echo "==> Build complete."
echo "    redis-server: $PWD/src/redis-server"
if [ -n "$modules" ]; then
  echo "    Module artifacts:"
  for name in $modules; do
    sos="$(find "modules/$name" -type f -name '*.so' \
        -not -path '*/venv/*' \
        -not -path '*/.cargo/*' \
        -not -path '*/site-packages/*' \
        -not -path '*/deps/*' 2>/dev/null || true)"
    if [ -n "$sos" ]; then
      printf "      %-20s " "$name:"; echo "$sos" | head -1
      echo "$sos" | tail -n +2 | sed 's|^|                           |'
    else
      printf "      %-20s (no .so found)\n" "$name:"
    fi
  done
fi

echo
echo "==> Refreshing redis-gen.conf via sync-redis-conf"
"$MAKE_BIN" --no-print-directory sync-redis-conf MODULES="$modules"

if [ -n "$failed" ]; then
  echo
  echo "ERROR: make build finished with module failure(s):$failed"
  exit 1
fi
