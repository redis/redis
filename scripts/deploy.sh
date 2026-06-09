#!/usr/bin/env bash
# deploy.sh — build (via scripts/build.sh) then copy artifacts to $(PREFIX).
#
# Usage:  scripts/deploy.sh [<name> ...|all|.|'*'|redis|none]
# Env:    PREFIX    install root (default /usr/local). Files land in:
#                     $PREFIX/bin/                  - redis-server, -cli, -benchmark
#                     $PREFIX/lib/redis/modules/    - per-module .so files
#         DESTDIR   optional staging root prepended to PREFIX (for packaging).
#         MAKE      make binary (defaults to `make`); only used when shelling
#                   into build.sh, which itself respects it.
#
# Tokens:
#   (no args) | all | . | '*'   build + install Redis + every cloned module
#   redis | none                 Redis only
#   <name> [<name> ...]          Redis + the listed modules
#
# Flow:
#   1. Delegate build to scripts/build.sh — same orchestration the user gets
#      from `make build` (failure collection, redis-gen.conf refresh, etc.).
#   2. After build, copy artifacts into place ourselves (NOT via each
#      module's `install` target). This keeps the install step a pure copy
#      and avoids depending on per-module Makefile install recipes.
#
# Failures during build are surfaced from build.sh; failures during copy are
# collected and reported at the end of this script.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)" || exit 1
. "$SCRIPT_DIR/lib/manifest.sh"
cd "$REPO_ROOT"

PREFIX="${PREFIX:-/usr/local}"
DESTDIR="${DESTDIR:-}"

cloned="$(cloned_modules)"
modules="$(resolve_modules "$*" "$cloned" "redis none")"

# ---------------------------------------------------------------------------
# Phase 1: build via the shared orchestrator. Pass the resolved module list
# verbatim so build.sh sees the same selection we intend to install.
# ---------------------------------------------------------------------------
echo "==> Building before deploy (delegating to scripts/build.sh)"
echo
# If $modules is empty (`none` token) we still want Redis core built — feed
# the literal "redis" token in that case so build.sh builds core only.
if [ -z "$modules" ]; then
  "$SCRIPT_DIR/build.sh" redis
else
  "$SCRIPT_DIR/build.sh" $modules
fi

# ---------------------------------------------------------------------------
# Phase 2: copy artifacts to $DESTDIR$PREFIX. Pure file ops, no recursive make.
# ---------------------------------------------------------------------------
INSTALL_BIN_DIR="$DESTDIR$PREFIX/bin"
INSTALL_MOD_DIR="$DESTDIR$PREFIX/lib/redis/modules"

echo
echo "==> Deploying to PREFIX=$PREFIX${DESTDIR:+ (DESTDIR=$DESTDIR)}"
echo

# Redis core binaries + the three symlinks that point to redis-server.
echo "==> Installing Redis core binaries to $INSTALL_BIN_DIR"
install -d "$INSTALL_BIN_DIR"
install -m 0755 src/redis-server     "$INSTALL_BIN_DIR/redis-server"
install -m 0755 src/redis-cli        "$INSTALL_BIN_DIR/redis-cli"
install -m 0755 src/redis-benchmark  "$INSTALL_BIN_DIR/redis-benchmark"
ln -sf redis-server "$INSTALL_BIN_DIR/redis-check-rdb"
ln -sf redis-server "$INSTALL_BIN_DIR/redis-check-aof"
ln -sf redis-server "$INSTALL_BIN_DIR/redis-sentinel"

# Per-module `.so` files. After scripts/build.sh, each cloned module has its
# .so copied to modules/<name>/<basename> by common.mk (the `cp $(TARGET_MODULE) ./`
# step in $(TARGET_MODULE)'s recipe). Read the manifest's `target_module:`
# field to know the exact basename for each module (handles redisjson →
# rejson.so) and copy from there.
failed=""
if [ -n "$modules" ]; then
  echo
  echo "==> Installing modules to $INSTALL_MOD_DIR"
  install -d "$INSTALL_MOD_DIR"
  for name in $modules; do
    target="$(manifest_field "$name" target_module)"
    if [ -z "$target" ]; then
      echo "==> [module] $name: no 'target_module' in modules.yaml — skipping"
      failed="$failed $name"
      continue
    fi
    so_basename="$(basename "$target")"
    so_src="modules/$name/$so_basename"
    if [ ! -f "$so_src" ]; then
      echo "==> [module] $name: built artifact not found at $so_src — skipping"
      failed="$failed $name"
      continue
    fi
    install -m 0755 "$so_src" "$INSTALL_MOD_DIR/$so_basename"
    echo "    [module] $name -> $INSTALL_MOD_DIR/$so_basename"
  done
fi

echo
echo "==> Deploy complete."
echo "    redis-server: $INSTALL_BIN_DIR/redis-server"
if [ -n "$modules" ]; then
  echo "    Module .so directory: $INSTALL_MOD_DIR/"
fi

if [ -n "$failed" ]; then
  echo
  echo "ERROR: deploy finished with module copy failure(s):$failed" >&2
  exit 1
fi
