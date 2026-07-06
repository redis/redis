#!/bin/sh
# sync-redis-conf.sh — (re)generate $REDIS_GEN_CONF from $REDIS_CONF + modules.yaml.
#
# Documented entry point is `make sync-redis-conf` (see the top-level
# Makefile). This script can also be invoked directly:
#
#   scripts/sync-redis-conf.sh                    # all manifest modules, real .so state
#   MODULES="redistimeseries redisjson" \
#     scripts/sync-redis-conf.sh                  # explicit subset
#   ASSUME_BUILT=1 scripts/sync-redis-conf.sh     # emit loadmodule even when .so missing
#
# Environment contract (all optional):
#   REDIS_CONF              path to source conf       (default: redis.conf)
#   REDIS_GEN_CONF          path to generated conf    (default: redis-full.conf)
#   MODULES                 space-separated module subset (default: every module
#                           in modules.yaml; pass an explicit list to restrict)
#   ASSUME_BUILT            "1" / "true" / "yes" → emit active loadmodule lines
#                           regardless of whether the .so is present on disk
#                           (used by `make tarball`)
#   MODULES_MANIFEST_FILE   manifest path             (default: modules/modules.yaml)
#   PREFIX                  installed modules directory. When set, switches to
#                           install mode: loadmodule paths are written as
#                           `$PREFIX/<basename>`. When unset (default), paths
#                           are kept as-is from modules.yaml (relative), so
#                           redis-full.conf works from the source/tarball root.
#
# Output is always rewritten in full and is atomic (write to sibling tmpfile,
# then rename). On any failure the temp file is cleaned up and the existing
# $REDIS_GEN_CONF is left untouched.

set -eu

SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)"
# shellcheck source=lib/manifest.sh
. "$SCRIPT_DIR/lib/manifest.sh"
cd "$REPO_ROOT"

REDIS_CONF="${REDIS_CONF:-redis.conf}"
REDIS_GEN_CONF="${REDIS_GEN_CONF:-redis-full.conf}"

# PREFIX: when set, switches to install mode — loadmodule paths are written as
# $PREFIX/<basename>. Used by `make deploy` (PREFIX = installed modules dir).
# When unset, dev mode: paths are kept as-is from modules.yaml (relative).
PREFIX="${PREFIX:-}"
PREFIX="${PREFIX%/}"

INSTALL_MODE=0
[ -n "$PREFIX" ] && INSTALL_MODE=1

# Translate a manifest `loadmodule:` value into the path written to the
# generated conf:
#   install mode   →  $PREFIX/<basename>   (PREFIX = installed modules dir)
#   dev mode       →  path as-is from modules.yaml (relative paths stay relative)
resolve_so_path() {
  if [ "$INSTALL_MODE" = "1" ]; then
    printf '%s\n' "$PREFIX/$(basename "$1")"
    return
  fi
  printf '%s\n' "$1"
}

# Mirror DOCKER_STRICT semantics from
# modules/docker-install-bundled-module-deps.sh so all boolean-ish env vars
# in this codebase accept the same forms.
is_truthy() {
  case "${1:-}" in
    1|true|TRUE|yes|YES) return 0 ;;
    *) return 1 ;;
  esac
}
ASSUME_BUILT_FLAG=0
if is_truthy "${ASSUME_BUILT:-}"; then
  ASSUME_BUILT_FLAG=1
fi

# Marker strings used both as input filters and output framing. Single
# source of truth so a future rename only happens here.
#
# CORE_BEGIN / CORE_END wrap the user-editable Redis-core configuration in
# $REDIS_CONF. `sync-redis-conf` extracts ONLY the content between these
# markers — anything outside them in $REDIS_CONF is ignored. This makes
# `apply-redis-conf` idempotent: an applied redis.conf still has these
# markers around its core section (plus an appended Modules section), so a
# subsequent sync re-extracts just the core and regenerates Modules fresh.
CORE_BEGIN="# >>> BEGIN: Redis-core config (DO NOT REMOVE THIS MARKER) <<<"
CORE_END="# <<< END: Redis-core config (DO NOT REMOVE THIS MARKER) >>>"
MODULES_BEGIN="# >>> BEGIN section: Modules (regenerated on every sync) <<<"
MODULES_END="# <<< END section: Modules <<<"
LOADMODULE_BEGIN="# >>> BEGIN: loadmodule paths (replaced by make deploy) <<<"
LOADMODULE_END="# <<< END: loadmodule paths <<<"
PRIVATE_BEGIN="# >>> BEGIN redis-gen-conf:private <<<"
PRIVATE_END="# <<< END redis-gen-conf:private <<<"
# Header line that marks the ## MODULES ## block in redis.conf. Stripped from
# the extracted core so the generated Modules section takes its place.
MODULES_PLACEHOLDER_HEADER="################################## MODULES #####################################"

if [ ! -f "$REDIS_CONF" ]; then
  echo "ERROR: $REDIS_CONF not found" >&2
  exit 1
fi
if [ ! -f "$MODULES_MANIFEST_FILE" ]; then
  echo "ERROR: $MODULES_MANIFEST_FILE not found" >&2
  exit 1
fi

# Resolve the requested module set.
#   unset / empty MODULES / all / . / '*'  → every manifest module (same
#                                             wildcard synonyms resolve_modules()
#                                             accepts for every other script)
#   MODULES=none                           → explicitly no modules (redis-only
#                                             build via build.sh)
#   MODULES=<names>                        → exactly those modules
default_used=0
requested_raw="${MODULES:-}"
_modules_stripped="$(printf '%s' "$requested_raw" | tr -d '[:space:]')"
case "$_modules_stripped" in
  ''|all|.|'*')
    requested="$(manifest_modules | tr '\n' ' ')"
    default_used=1
    ;;
  none)
    requested=""
    ;;
  *)
    requested="$requested_raw"
    ;;
esac
unset _modules_stripped
# Collapse runs of whitespace to single spaces, trim ends.
requested="$(printf '%s\n' "$requested" | manifest_join_words)"

# One-shot extract of every `name<TAB>loadmodule` pair from the manifest, so
# subsequent lookups don't re-scan modules.yaml. The original recipe called
# awk three times per module, which became real overhead as the list grew —
# now we do a single pass plus O(1)-ish lookups against the tiny cache.
# Bash 3.2 (macOS default) lacks `declare -A`, so a temp file + awk is the
# portable cache.
LOOKUP_FILE="$(mktemp -t syncrediscacheXXXXXX)"
trap 'rm -f "$LOOKUP_FILE" "${tmp_out:-}"' EXIT

awk '
  BEGIN { cur = "" }
  /^[ \t]*-[ \t]*name:[ \t]*/ {
    line = $0
    sub(/^[ \t]*-[ \t]*name:[ \t]*/, "", line)
    sub(/[ \t]+$/, "", line)
    cur = line
    next
  }
  cur != "" && /^[ \t]+loadmodule:[ \t]*/ {
    v = $0
    sub(/^[ \t]+loadmodule:[ \t]*/, "", v)
    sub(/[ \t]+$/, "", v)
    print cur "\t" v
  }
' "$MODULES_MANIFEST_FILE" > "$LOOKUP_FILE"

lookup_so() {
  awk -F'\t' -v want="$1" '$1 == want { print $2; exit }' "$LOOKUP_FILE"
}

active=""
missing=""
bad_manifest=""
for name in $requested; do
  so="$(lookup_so "$name")"
  if [ -z "$so" ]; then
    bad_manifest="${bad_manifest}${bad_manifest:+ }${name}"
  elif [ -f "$(resolve_so_path "$so")" ] || [ "$ASSUME_BUILT_FLAG" = "1" ]; then
    active="${active}${active:+ }${name}"
  else
    missing="${missing}${missing:+ }${name}"
  fi
done

# Filters reused below.
#
# strip_block: "skip lines between BEGIN/END markers, including the markers
# themselves". Nesting is NOT supported: a nested BEGIN would simply re-arm
# `skip`, an inner END would re-clear it. Files without markers are passed
# through verbatim, so the filter is fully opt-in per module. Used to strip
# per-module redis-gen-conf:private blocks from module.conf.
strip_block() {
  # strip_block <begin-marker> <end-marker> <file>
  awk -v begin="$1" -v end="$2" '
    $0 == begin { skip = 1; next }
    $0 == end   { skip = 0; next }
    !skip       { print }
  ' "$3"
}

# extract_section: emit ONLY the lines between BEGIN and END markers
# (exclusive). Validates that exactly one BEGIN and one END appear in the
# right order; errors loudly otherwise. Used to pull the Redis-core section
# out of $REDIS_CONF — anything outside the markers (e.g. an appended
# Modules section after a previous apply) is ignored.
extract_section() {
  # extract_section <begin-marker> <end-marker> <file>
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

# Display strings (the long-form list is replaced by <none> when empty so
# the file's header is grep-friendly and readable).
display_or_none() {
  if [ -z "$1" ]; then printf '%s' '<none>'; else printf '%s' "$1"; fi
}
active_display="$(display_or_none "$active")"
missing_display="$(display_or_none "$missing")"
bad_display="$(display_or_none "$bad_manifest")"
if [ "$default_used" = "1" ]; then
  requested_display="<all manifest modules>"
else
  requested_display="$requested"
fi

generated_at="$(date -u +'%Y-%m-%dT%H:%M:%SZ')"

# Atomic write: emit everything to a sibling tmpfile, then rename. Trap the
# tmpfile so a failure mid-emit leaves the existing redis-full.conf intact.
# (The trap above already covers $LOOKUP_FILE; we extend it here to cover
# the output tmp as well.)
tmp_out="$(mktemp "${REDIS_GEN_CONF}.tmp.XXXXXX")"
trap 'rm -f "$LOOKUP_FILE" "$tmp_out"' EXIT

{
  cat <<EOF
# =============================================================================
# redis-full.conf — auto-generated by \`make sync-redis-conf\`
# =============================================================================
#
# Untracked, regenerated whenever \`make modules-update\` or \`make\`
# runs (both call \`sync-redis-conf\` at the end). Two sections:
#
#   1. Redis-core config — the content extracted from $REDIS_CONF between
#      the marker lines:
#        $CORE_BEGIN
#        $CORE_END
#      Anything outside those markers in $REDIS_CONF is ignored. Edit
#      $REDIS_CONF (inside the markers) to change Redis-core defaults; the
#      next sync picks them up.
#   2. Modules — only the modules requested by the caller (via the MODULES
#      variable; defaults to all manifest modules when invoked standalone).
#      Each requested module appears as an active \`loadmodule\` + inlined
#      module.conf if its .so is present, or as a commented placeholder
#      otherwise. Modules not in the request list are omitted entirely.
#
# Do NOT edit redis-full.conf — your edits will be overwritten.
#
# Generated:    $generated_at
# Requested:    $requested_display
# Active:       $active_display
# Missing .so:  $missing_display
# Bad manifest: $bad_display
# =============================================================================

# ============================================================================
# Redis-core configuration
# ----------------------------------------------------------------------------
# The block below, between the BEGIN/END markers, is the user-editable
# Redis-core configuration.
#
# \`make sync-redis-conf\` extracts ONLY the content between BEGIN/END to build
# redis-full.conf. Anything outside the markers (including this banner) is
# ignored on every regeneration, so the banner is safe to keep here.
#
# The \`## MODULES ##\` section in redis.conf is a placeholder — sync-redis-conf
# strips it from the extracted core and regenerates it with real loadmodule
# lines + per-module config. To strip that generated section back out, run
# \`make apply-redis-conf revert\`.
# ============================================================================

$CORE_BEGIN
EOF
  # Extract the core section, stripping the ## MODULES ## placeholder block so
  # the generated Modules section takes its place below instead of duplicating.
  # extract_section is staged to a temp first (not piped straight into awk):
  # POSIX sh has no `pipefail`, so a piped extract_section failure (e.g. a
  # missing marker → exit 2) would be masked by awk exiting 0, silently
  # emitting a truncated conf. The redirect lets `set -e` abort instead.
  _core_tmp="$(mktemp)"
  extract_section "$CORE_BEGIN" "$CORE_END" "$REDIS_CONF" > "$_core_tmp"
  awk -v hdr="$MODULES_PLACEHOLDER_HEADER" '
      $0 == hdr           { skip=1; next }
      skip && /^#{8,} [A-Z]/ { skip=0 }
      skip                { next }
                          { print }
    ' "$_core_tmp"
  rm -f "$_core_tmp"
  cat <<EOF
$CORE_END

$MODULES_PLACEHOLDER_HEADER
$MODULES_BEGIN

EOF

  # `loadmodule` lines (or commented placeholders) are wrapped in markers so
  # `make deploy` can replace just this block with installed paths without
  # regenerating the entire conf. File existence is tested against the on-disk
  # path (REPO_ROOT-relative). In dev mode paths stay relative (as in
  # modules.yaml); in install mode they become $PREFIX/<basename>.
  echo "$LOADMODULE_BEGIN"
  for name in $requested; do
    so="$(lookup_so "$name")"
    if [ -z "$so" ]; then
      printf "# %s: 'loadmodule' field missing in modules.yaml\n" "$name"
      continue
    fi
    so_full="$(resolve_so_path "$so")"
    # Check existence at the path we're about to write into the conf, so the
    # generated file is a truthful manifest of what redis-server can load.
    if [ -f "$so_full" ] || [ "$ASSUME_BUILT_FLAG" = "1" ]; then
      printf "loadmodule %s\n" "$so_full"
    elif [ "$INSTALL_MODE" = "1" ]; then
      printf "# %s: not installed (%s absent — run 'make deploy %s PREFIX=%s')\n" \
        "$name" "$so_full" "$name" "$PREFIX"
      printf "# loadmodule %s\n" "$so_full"
    else
      printf "# %s: not built (%s absent — run 'make %s')\n" \
        "$name" "$so_full" "$name"
      printf "# loadmodule %s\n" "$so_full"
    fi
  done
  echo "$LOADMODULE_END"
  echo

  # Per-module config blocks. Each block is wrapped in its own BEGIN/END
  # markers so it's trivially diff-able and locatable in the generated file.
  for name in $requested; do
    so="$(lookup_so "$name")"
    conf="modules/$name/src/module.conf"
    so_full=""
    [ -n "$so" ] && so_full="$(resolve_so_path "$so")"
    printf "# >>> BEGIN module: %s <<<\n" "$name"
    if [ -n "$so_full" ] && { [ -f "$so_full" ] || [ "$ASSUME_BUILT_FLAG" = "1" ]; }; then
      if [ -f "$conf" ]; then
        strip_block "$PRIVATE_BEGIN" "$PRIVATE_END" "$conf"
      else
        printf "# (no %s found)\n" "$conf"
      fi
    else
      printf "# (module not built — directives elided so redis-server won't\n"
      printf "#  reject unknown config; run 'make %s' to enable)\n" "$name"
    fi
    printf "# <<< END module: %s <<<\n\n" "$name"
  done

  echo "$MODULES_END"
} > "$tmp_out"

mv "$tmp_out" "$REDIS_GEN_CONF"
rm -f "$LOOKUP_FILE"
trap - EXIT

echo "==> Wrote $REDIS_GEN_CONF"
echo "    requested:    $requested_display"
echo "    active:       $active_display"
echo "    bad manifest: $bad_display"

# Make absent .so files loud. Color only on TTYs so log/CI output stays clean.
if [ -n "$missing" ]; then
  if [ -t 1 ]; then
    bold=$'\033[1m'; yellow=$'\033[33m'; reset=$'\033[0m'
  else
    bold=""; yellow=""; reset=""
  fi
  printf '\n%s%sWARNING: missing .so for: %s%s\n' "$bold" "$yellow" "$missing" "$reset"
  if [ "$INSTALL_MODE" = "1" ]; then
    printf '%s         run: make deploy %s PREFIX=%s%s\n\n' "$yellow" "$missing" "$PREFIX" "$reset"
  else
    printf '%s         run: make %s%s\n\n' "$yellow" "$missing" "$reset"
  fi
else
  echo "    missing .so:  <none>"
fi
