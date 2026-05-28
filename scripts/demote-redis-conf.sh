#!/usr/bin/env bash
# demote-redis-conf.sh — strip the auto-generated Modules section from
# $REDIS_CONF, leaving just the Redis-core section (with its BEGIN/END
# markers and banner intact).
#
# Documented entry point is `make demote-redis-conf`. Inverse of
# `make promote-redis-conf`: where promote appends an auto-generated Modules
# block onto $REDIS_CONF, demote removes it again.
#
# Usage:
#   scripts/demote-redis-conf.sh                    # operate on default redis.conf
#   REDIS_CONF=/path/to/some.conf \
#     scripts/demote-redis-conf.sh                  # explicit file
#
# Environment contract:
#   REDIS_CONF   path to source conf (default: redis.conf)
#
# What it does:
#   1. Validates $REDIS_CONF contains the Redis-core BEGIN/END markers
#      written by `sync-redis-conf`.
#   2. Extracts the content between those markers (the "core" config).
#   3. Atomically rewrites $REDIS_CONF as: banner + BEGIN marker + core
#      content + END marker + banner. Anything outside the markers (an
#      auto-generated header, an appended Modules section, stray content)
#      is discarded.
#
# Idempotent: re-running on an already-demoted file is a no-op modulo
# banner-cosmetic regeneration.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)" || exit 1
REPO_ROOT="${REPO_ROOT:-$(cd -- "$SCRIPT_DIR/.." && pwd)}"
cd "$REPO_ROOT"

REDIS_CONF="${REDIS_CONF:-redis.conf}"

if [ ! -f "$REDIS_CONF" ]; then
  echo "ERROR: $REDIS_CONF not found" >&2
  exit 1
fi

# Must stay in lockstep with sync-redis-conf.sh — these two scripts both own
# the marker contract. Keep the strings byte-identical.
CORE_BEGIN="# >>> BEGIN: Redis-core config (DO NOT REMOVE THIS MARKER) <<<"
CORE_END="# <<< END: Redis-core config (DO NOT REMOVE THIS MARKER) >>>"

# Extract content between CORE_BEGIN and CORE_END (exclusive). Errors if
# markers are missing, duplicated, or out of order. Same shape as the
# extract_section in sync-redis-conf.sh — kept inline here so demote stays
# independent of sync (it doesn't need the manifest, modules, prefix, etc.).
extract_section() {
  awk -v begin="$1" -v end="$2" -v file="$3" '
    $0 == begin {
      if (in_section) {
        printf "ERROR: %s: nested or duplicate BEGIN marker at line %d\n", file, NR > "/dev/stderr"
        exit 2
      }
      begin_count++; in_section = 1; next
    }
    $0 == end {
      if (!in_section) {
        printf "ERROR: %s: END marker without matching BEGIN at line %d\n", file, NR > "/dev/stderr"
        exit 2
      }
      end_count++; in_section = 0; next
    }
    in_section { print }
    END {
      if (begin_count == 0) {
        printf "ERROR: %s: missing BEGIN marker (%s)\n", file, begin > "/dev/stderr"
        printf "       Cannot demote — file does not contain a Redis-core section.\n" > "/dev/stderr"
        exit 2
      }
      if (end_count == 0) {
        printf "ERROR: %s: missing END marker (%s)\n", file, end > "/dev/stderr"
        exit 2
      }
      if (begin_count > 1) {
        printf "ERROR: %s: duplicate BEGIN marker (%d occurrences)\n", file, begin_count > "/dev/stderr"
        exit 2
      }
      if (end_count > 1) {
        printf "ERROR: %s: duplicate END marker (%d occurrences)\n", file, end_count > "/dev/stderr"
        exit 2
      }
    }
  ' "$3"
}

# Two tmpfiles so the extract step can fail cleanly without leaving a
# half-written redis.conf: first stage the extracted core, then assemble the
# final file from it, then atomically move into place.
tmp_extract="$(mktemp "${REDIS_CONF}.extract.XXXXXX")"
tmp_out="$(mktemp "${REDIS_CONF}.tmp.XXXXXX")"
trap 'rm -f "$tmp_extract" "$tmp_out"' EXIT

echo "==> Extracting Redis-core section from $REDIS_CONF"
extract_section "$CORE_BEGIN" "$CORE_END" "$REDIS_CONF" > "$tmp_extract"

# Detect whether the source file had a Modules section appended (i.e. was
# previously promoted). Purely cosmetic — drives the final status message.
had_modules=0
if grep -q '^# >>> BEGIN section: Modules' "$REDIS_CONF"; then
  had_modules=1
fi

{
  cat <<EOF
# ============================================================================
# Redis-core configuration
# ----------------------------------------------------------------------------
# The block below, between the BEGIN/END markers, is the user-editable
# Redis-core configuration.
#
# \`make sync-redis-conf\` extracts ONLY the content between BEGIN/END to build
# redis-gen.conf. Anything outside the markers (including this banner) is
# ignored on every regeneration, so the banner is safe to keep here.
#
# Module load lines are NOT placed inside the markers — they belong in the
# auto-generated Modules section below. To strip that section back out, run
# \`make demote-redis-conf\`.
# ============================================================================

$CORE_BEGIN
EOF
  cat "$tmp_extract"
  cat <<EOF
$CORE_END
EOF
} > "$tmp_out"

mv "$tmp_out" "$REDIS_CONF"
trap - EXIT
rm -f "$tmp_extract"

if [ "$had_modules" = "1" ]; then
  echo "==> $REDIS_CONF demoted — Modules section removed; Redis-core section preserved"
else
  echo "==> $REDIS_CONF already core-only — no Modules section to remove (banner regenerated)"
fi
