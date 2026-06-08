#!/usr/bin/env bash
# deploy.sh — install Redis (src/) + selected modules to $(PREFIX).
#
# Usage:  scripts/deploy.sh [<name> ...|all|.|'*'|redis|none]
# Env:    PREFIX    install root (default /usr/local). Files land in:
#                     $PREFIX/bin/                  - redis-server, -cli, -benchmark
#                     $PREFIX/lib/redis/modules/    - per-module .so files
#         DESTDIR   optional staging root prepended to PREFIX (for packaging).
#         MAKE      make binary (defaults to `make`)
#
# Tokens:
#   (no args) | all | . | '*'   install Redis + every cloned module
#   redis | none                 install Redis only
#   <name> [<name> ...]          install Redis + the listed modules
#
# Each install invokes the same `make install` recipes that `make install`
# would (src/Makefile and modules/common.mk), so the end-state is identical
# to the legacy target — just with explicit per-module selection and
# PREFIX forwarding from the Make wrapper.
# Failures are collected and reported at the end.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)" || exit 1
. "$SCRIPT_DIR/lib/manifest.sh"
cd "$REPO_ROOT"

MAKE_BIN="${MAKE:-make}"
PREFIX="${PREFIX:-/usr/local}"
DESTDIR="${DESTDIR:-}"

cloned="$(cloned_modules)"
modules="$(resolve_modules "$*" "$cloned" "redis none")"

echo "==> Deploying to PREFIX=$PREFIX${DESTDIR:+ (DESTDIR=$DESTDIR)}"

echo
echo "==> Installing Redis core (src/)"
if ! "$MAKE_BIN" -C src install PREFIX="$PREFIX" DESTDIR="$DESTDIR"; then
  echo
  echo "ERROR: Redis core install failed (make -C src install)." >&2
  exit 1
fi

failed=""
if [ -z "$modules" ]; then
  echo
  if [ -z "$cloned" ]; then
    echo "==> No cloned modules under modules/*/src, Redis-only deploy"
  else
    echo "==> Module installs skipped by request"
  fi
else
  echo
  echo "==> Installing modules to $DESTDIR$PREFIX/lib/redis/modules:"
  echo "   $modules"
  for name in $modules; do
    echo
    echo "==> [module] $name (modules/$name)"
    if ! "$MAKE_BIN" -C "modules/$name" -f "$REPO_ROOT/modules/common.mk" install \
        PREFIX="$PREFIX" DESTDIR="$DESTDIR" \
        REDIS_SERVER="$REPO_ROOT/src/redis-server"; then
      failed="$failed $name"
      echo "==> [module] $name: FAILED (continuing with remaining modules)"
    fi
  done
  if [ -n "$failed" ]; then
    echo
    echo "==> WARNING: The following module(s) failed to install:$failed"
  fi
fi

echo
echo "==> Deploy complete."
echo "    redis-server: $DESTDIR$PREFIX/bin/redis-server"
if [ -n "$modules" ]; then
  echo "    Module .so directory: $DESTDIR$PREFIX/lib/redis/modules/"
fi

if [ -n "$failed" ]; then
  echo
  echo "ERROR: make deploy finished with module failure(s):$failed"
  exit 1
fi
