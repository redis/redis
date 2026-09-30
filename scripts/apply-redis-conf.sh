#!/bin/sh
# apply-redis-conf.sh — regenerate redis-full.conf and overwrite redis.conf
# with it, so `./src/redis-server redis.conf` auto-loads the bundled modules.
#
# Used by the tarball build (scripts/tarball.sh) so a release tarball ships a
# single redis.conf that already contains all module configuration. Not part
# of the normal local flow — for local runs just use `redis-server redis-full.conf`.
#
# Usage:
#   scripts/apply-redis-conf.sh                       # apply
#   MODULES="redistimeseries redisjson" \
#     scripts/apply-redis-conf.sh                     # explicit subset
#   ASSUME_BUILT=1 scripts/apply-redis-conf.sh        # emit loadmodule even
#                                                       # when .so missing
#
# Env: REDIS_CONF, REDIS_GEN_CONF, MODULES, ASSUME_BUILT, MODULES_MANIFEST_FILE,
#      PREFIX — all forwarded to sync-redis-conf.sh unchanged.
#
# Idempotent: sync-redis-conf strips any previously appended Modules block
# before re-appending a fresh one, so re-applying never duplicates it. Use
# `git checkout -- redis.conf` to fully restore the original.

set -eu

SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)" || exit 1
REPO_ROOT="${REPO_ROOT:-$(cd -- "$SCRIPT_DIR/.." && pwd)}"
cd "$REPO_ROOT"

REDIS_CONF="${REDIS_CONF:-redis.conf}"
REDIS_GEN_CONF="${REDIS_GEN_CONF:-redis-full.conf}"

if [ "$#" -ne 0 ]; then
  echo "ERROR: apply-redis-conf.sh takes no arguments (got: $*)" >&2
  exit 1
fi

if [ ! -f "$REDIS_CONF" ]; then
  echo "ERROR: $REDIS_CONF not found" >&2
  exit 1
fi

echo "==> Regenerating $REDIS_GEN_CONF from $REDIS_CONF"
"$SCRIPT_DIR/sync-redis-conf.sh"

if [ ! -f "$REDIS_GEN_CONF" ]; then
  echo "ERROR: sync-redis-conf did not produce $REDIS_GEN_CONF; refusing to apply" >&2
  exit 1
fi

echo "==> Applying $REDIS_GEN_CONF -> $REDIS_CONF (overwriting)"
mv -f "$REDIS_GEN_CONF" "$REDIS_CONF"
echo "==> $REDIS_CONF now contains the generated module config"
echo "    ('git checkout -- $REDIS_CONF' reverts the file)"
