#!/usr/bin/env bash
# apply-redis-conf.sh — apply the auto-generated Modules section into
# $REDIS_CONF, or revert it.
#
# Documented entry point is `make apply-redis-conf`. Two modes:
#
#   apply  (default)
#     Regenerate $REDIS_GEN_CONF via scripts/sync-redis-conf.sh, then
#     overwrite $REDIS_CONF with it. After this, `./src/redis-server
#     redis.conf` auto-loads the bundled modules — no need to point at
#     redis-gen.conf. Typical use: after `tar xzf redis-<tag>.tar.gz &&
#     make build` from a release tarball.
#
#   revert
#     Inverse. Strip the auto-generated Modules section back out of
#     $REDIS_CONF, leaving just the Redis-core section (with its
#     BEGIN/END markers and banner intact).
#
# Usage:
#   scripts/apply-redis-conf.sh                       # apply
#   scripts/apply-redis-conf.sh revert                # revert
#   MODULES="redistimeseries redisjson" \
#     scripts/apply-redis-conf.sh                     # apply, explicit subset
#   ASSUME_BUILT=1 scripts/apply-redis-conf.sh        # apply, emit loadmodule
#                                                       # even when .so missing
#
# Environment contract (apply mode): REDIS_CONF, REDIS_GEN_CONF, MODULES,
# ASSUME_BUILT, MODULES_MANIFEST_FILE, PREFIX — all forwarded to
# sync-redis-conf.sh unchanged. Revert mode reads only REDIS_CONF.
#
# Both modes are safe to re-run:
#   - apply  is idempotent because sync-redis-conf extracts only the content
#            between the `# >>> BEGIN: Redis-core config ... <<<` and matching
#            END markers; a previously applied Modules section is simply
#            ignored on the next pass and replaced with a fresh one.
#   - revert is idempotent — re-running on an already-core-only file just
#            regenerates the banner.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)" || exit 1
REPO_ROOT="${REPO_ROOT:-$(cd -- "$SCRIPT_DIR/.." && pwd)}"
cd "$REPO_ROOT"

REDIS_CONF="${REDIS_CONF:-redis.conf}"
REDIS_GEN_CONF="${REDIS_GEN_CONF:-redis-gen.conf}"

# Parse positional args for the revert toggle.
REVERT=0
for arg in "$@"; do
  case "$arg" in
    revert|--revert) REVERT=1 ;;
    *)
      echo "ERROR: unknown arg '$arg' (expected 'revert' or no args)" >&2
      exit 1
      ;;
  esac
done

if [ ! -f "$REDIS_CONF" ]; then
  echo "ERROR: $REDIS_CONF not found" >&2
  exit 1
fi

# ---------------------------------------------------------------------------
# APPLY mode
# ---------------------------------------------------------------------------
apply_mode() {
  echo "==> Regenerating $REDIS_GEN_CONF from $REDIS_CONF"
  "$SCRIPT_DIR/sync-redis-conf.sh"

  if [ ! -f "$REDIS_GEN_CONF" ]; then
    echo "ERROR: sync-redis-conf did not produce $REDIS_GEN_CONF; refusing to apply" >&2
    exit 1
  fi

  echo "==> Applying $REDIS_GEN_CONF -> $REDIS_CONF (overwriting)"
  mv -f "$REDIS_GEN_CONF" "$REDIS_CONF"
  echo "==> $REDIS_CONF now contains the generated module config"
  echo "    ($REDIS_GEN_CONF removed; 'git diff $REDIS_CONF' to inspect)"
  echo "    ('make apply-redis-conf revert' strips the Modules section back out;"
  echo "     'git checkout -- $REDIS_CONF' reverts the file entirely)"
}

# ---------------------------------------------------------------------------
# REVERT mode — strip the auto-generated Modules section
# ---------------------------------------------------------------------------

# Must stay in lockstep with sync-redis-conf.sh — these two scripts both own
# the marker contract. Keep the strings byte-identical.
CORE_BEGIN="# >>> BEGIN: Redis-core config (DO NOT REMOVE THIS MARKER) <<<"
CORE_END="# <<< END: Redis-core config (DO NOT REMOVE THIS MARKER) >>>"

# Extract content between CORE_BEGIN and CORE_END (exclusive). Errors if
# markers are missing, duplicated, or out of order. Same shape as the
# extract_section in sync-redis-conf.sh — kept inline so revert stays
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
        printf "       Cannot revert — file does not contain a Redis-core section.\n" > "/dev/stderr"
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

revert_mode() {
  # Two tmpfiles so the extract step can fail cleanly without leaving a
  # half-written redis.conf: first stage the extracted core, then assemble
  # the final file from it, then atomically move into place.
  local tmp_extract tmp_out
  tmp_extract="$(mktemp "${REDIS_CONF}.extract.XXXXXX")"
  tmp_out="$(mktemp "${REDIS_CONF}.tmp.XXXXXX")"
  trap 'rm -f "$tmp_extract" "$tmp_out"' EXIT

  echo "==> Extracting Redis-core section from $REDIS_CONF"
  extract_section "$CORE_BEGIN" "$CORE_END" "$REDIS_CONF" > "$tmp_extract"

  # Detect whether the source file had a Modules section appended (i.e. was
  # previously applied). Purely cosmetic — drives the final status message.
  local had_modules=0
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
# \`make apply-redis-conf revert\`.
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
    echo "==> $REDIS_CONF reverted — Modules section removed; Redis-core section preserved"
  else
    echo "==> $REDIS_CONF already core-only — no Modules section to remove (banner regenerated)"
  fi
}

if [ "$REVERT" = "1" ]; then
  revert_mode
else
  apply_mode
fi
