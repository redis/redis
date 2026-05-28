#!/usr/bin/env bash
# promote-redis-conf.sh — regenerate redis-gen.conf, then overwrite redis.conf with it.
#
# Documented entry point is `make promote-redis-conf`. This script is
# DESTRUCTIVE on $REDIS_CONF (typically the tracked redis.conf in the repo):
# it runs scripts/sync-redis-conf.sh to produce $REDIS_GEN_CONF, then renames
# $REDIS_GEN_CONF onto $REDIS_CONF. Use it when you want
# `./src/redis-server redis.conf` to "just work" with the bundled modules
# pre-loaded — typically after `tar xzf redis-<tag>.tar.gz && make build` from
# a release tarball.
#
# Usage:
#   scripts/promote-redis-conf.sh                    # all manifest modules
#   MODULES="redistimeseries redisjson" \
#     scripts/promote-redis-conf.sh                  # explicit subset
#   ASSUME_BUILT=1 scripts/promote-redis-conf.sh     # emit loadmodule even when .so missing
#
# Env contract is identical to scripts/sync-redis-conf.sh — REDIS_CONF,
# REDIS_GEN_CONF, MODULES, ASSUME_BUILT, MODULES_MANIFEST_FILE are all
# forwarded unchanged.
#
# Safe to re-run. `sync-redis-conf` extracts only the content between the
# `# >>> BEGIN: Redis-core config (DO NOT REMOVE THIS MARKER) <<<` and
# matching END markers in $REDIS_CONF, ignoring everything else — so a
# previously promoted Modules section in $REDIS_CONF is simply ignored on
# the next pass and replaced with a freshly regenerated one.
#
# To strip the appended Modules section back out of $REDIS_CONF, run
# `make demote-redis-conf` (or `git checkout -- $REDIS_CONF` to revert any
# in-flight core edits too).

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)" || exit 1
REPO_ROOT="${REPO_ROOT:-$(cd -- "$SCRIPT_DIR/.." && pwd)}"
cd "$REPO_ROOT"

REDIS_CONF="${REDIS_CONF:-redis.conf}"
REDIS_GEN_CONF="${REDIS_GEN_CONF:-redis-gen.conf}"

if [ ! -f "$REDIS_CONF" ]; then
  echo "ERROR: $REDIS_CONF not found" >&2
  exit 1
fi

echo "==> Regenerating $REDIS_GEN_CONF from $REDIS_CONF"
"$SCRIPT_DIR/sync-redis-conf.sh"

if [ ! -f "$REDIS_GEN_CONF" ]; then
  echo "ERROR: sync-redis-conf did not produce $REDIS_GEN_CONF; refusing to promote" >&2
  exit 1
fi

echo "==> Promoting $REDIS_GEN_CONF -> $REDIS_CONF (overwriting)"
mv -f "$REDIS_GEN_CONF" "$REDIS_CONF"
echo "==> $REDIS_CONF now contains the generated module config"
echo "    ($REDIS_GEN_CONF removed; 'git diff $REDIS_CONF' to inspect)"
echo "    ('make demote-redis-conf' strips the Modules section back out;"
echo "     'git checkout -- $REDIS_CONF' reverts the file entirely)"
