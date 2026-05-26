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
# Refuses to run if $REDIS_CONF already contains a generated module section
# (running again would nest another section inside it). Restore $REDIS_CONF
# from git first and re-run.

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

# sync-redis-conf wraps its module output in this marker. If REDIS_CONF
# already carries it, a previous promote has already run; another pass would
# nest sections (since sync only strips the legacy auto-managed block, not
# the new section markers it writes itself).
if grep -q '^# >>> BEGIN section: Modules' "$REDIS_CONF"; then
  echo "ERROR: $REDIS_CONF already contains a generated Modules section." >&2
  echo "       Running promote-redis-conf again would duplicate it." >&2
  echo "       Restore $REDIS_CONF from git (e.g. 'git checkout -- $REDIS_CONF')" >&2
  echo "       and re-run." >&2
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
echo "    ($REDIS_GEN_CONF removed; 'git diff $REDIS_CONF' to inspect; 'git checkout -- $REDIS_CONF' to revert)"
