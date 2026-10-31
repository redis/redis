#!/bin/sh
# uninstall.sh — remove what scripts/deploy.sh installed under $PREFIX.
#
# Usage:  scripts/uninstall.sh [<name> ...|all|.|'*'|redis|none]
# Env:    PREFIX    install root (default /usr/local). Files removed from:
#                     $PREFIX/bin/                  - redis-server, -cli,
#                                                     -benchmark + the three
#                                                     redis-server symlinks
#                     $PREFIX/lib/redis/modules/    - per-module .so files
#         PROG_SUFFIX  suffix the core programs were installed with
#                   (see `make PROG_SUFFIX=…`).
#         DESTDIR   optional staging root prepended to PREFIX (for packaging).
#
# Tokens (same grammar as deploy.sh):
#   (no args) | all | . | '*'   core + every module in modules.yaml
#   redis | none                 core only
#   <name> [<name> ...]          core + the listed modules
#
# Unlike deploy.sh the default set is every *manifest* module, not every
# *cloned* one: uninstall has to clean up after installs made from a tree that
# may since have been cleaned. rm -f on a path that was never installed is a
# no-op, so over-listing is free.
#
# Nothing outside $DESTDIR$PREFIX is touched — the in-tree redis.conf /
# redis-full.conf are left as they are.

set -eu

SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)" || exit 1
. "$SCRIPT_DIR/lib/manifest.sh"
cd "$REPO_ROOT"

PREFIX="${PREFIX:-/usr/local}"
DESTDIR="${DESTDIR:-}"
PROG_SUFFIX="${PROG_SUFFIX:-}"

modules="$(resolve_modules "$*" "$(manifest_modules)" "redis none")"

INSTALL_BIN_DIR="$DESTDIR$PREFIX/bin"
INSTALL_MOD_DIR="$DESTDIR$PREFIX/lib/redis/modules"

echo "==> Uninstalling from PREFIX=$PREFIX${DESTDIR:+ (DESTDIR=$DESTDIR)}"
echo

for f in redis-server redis-cli redis-benchmark \
         redis-check-rdb redis-check-aof redis-sentinel; do
  rm -f "$INSTALL_BIN_DIR/$f$PROG_SUFFIX"
done
echo "==> Removed Redis core binaries from $INSTALL_BIN_DIR"

if [ -n "$modules" ]; then
  echo
  echo "==> Removing modules from $INSTALL_MOD_DIR"
  for name in $modules; do
    target="$(manifest_field "$name" target_module)"
    [ -z "$target" ] && continue
    so="$INSTALL_MOD_DIR/$(basename "$target")"
    rm -f "$so"
    echo "    [module] $name -> $so"
  done
  # Prune the directories we created in deploy, but only when empty — a
  # user's own files under lib/redis are none of our business.
  rmdir "$INSTALL_MOD_DIR" "$DESTDIR$PREFIX/lib/redis" 2>/dev/null || true
fi

echo
echo "==> Uninstall complete."
