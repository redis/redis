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
# The generated Modules section is fenced by MODULES_BEGIN / MODULES_END.
# `redis.conf` itself carries no markers: sync emits it verbatim and appends
# the Modules block after it. Regeneration stays idempotent by stripping any
# previously-appended MODULES_BEGIN..MODULES_END block before re-appending
# (so applying twice never duplicates it).
MODULES_BEGIN="# >>> BEGIN section: Modules (regenerated on every sync) <<<"
MODULES_END="# <<< END section: Modules <<<"
LOADMODULE_BEGIN="# >>> BEGIN: loadmodule paths (replaced by make deploy) <<<"
LOADMODULE_END="# <<< END: loadmodule paths <<<"
# Private-block markers in a module's module.conf. Matched by PREFIX (the
# trailing "<<<"/">>>" token is ignored) so modules that wrote either form are
# handled uniformly — stripping is best-effort and never aborts the build.
PRIVATE_BEGIN="# >>> BEGIN redis-gen-conf:private"
PRIVATE_END="# <<< END redis-gen-conf:private"

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
# strip_modules_block: remove the [MODULES_BEGIN..MODULES_END] block (markers
# included) from redis.conf and print the rest. A file with no markers passes
# through verbatim. Marker structure is VALIDATED — malformed input is a hard
# error (exit 2): a second BEGIN before END, an END with no BEGIN, or a BEGIN
# left open at EOF. This is the sole idempotency guard for the Modules block,
# and `apply-redis-conf` may overwrite redis.conf with the output, so it must
# never silently truncate. Safe to be strict here: sync itself generates these
# markers, so they are always well-formed.
strip_modules_block() {
  # strip_modules_block <file>
  awk -v begin="$MODULES_BEGIN" -v end="$MODULES_END" -v file="$1" '
    $0 == begin {
      if (skip) { printf "ERROR: %s: nested/duplicate Modules BEGIN at line %d\n", file, NR > "/dev/stderr"; exit 2 }
      skip = 1; next
    }
    $0 == end {
      if (!skip) { printf "ERROR: %s: Modules END without matching BEGIN at line %d\n", file, NR > "/dev/stderr"; exit 2 }
      skip = 0; next
    }
    !skip { print }
    END { if (skip) { printf "ERROR: %s: Modules BEGIN without matching END\n", file > "/dev/stderr"; exit 2 } }
  ' "$1"
}

# strip_private_block: remove a module.conf's redis-gen-conf:private block and
# print the rest. Markers are matched by PREFIX ($PRIVATE_BEGIN/$PRIVATE_END —
# the trailing token is ignored), so modules that wrote ">>>" vs "<<<" are both
# handled. Best-effort/LENIENT: module.conf is module-authored, so a malformed
# or missing block just leaves those lines in place — it never aborts the build.
strip_private_block() {
  # strip_private_block <file>
  awk -v begin="$PRIVATE_BEGIN" -v end="$PRIVATE_END" '
    index($0, begin) == 1 { skip = 1; next }
    index($0, end)   == 1 { skip = 0; next }
    !skip { print }
  ' "$1"
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
  # Emit the user's Redis-core config (all of $REDIS_CONF) verbatim first,
  # minus any Modules block a previous \`apply-redis-conf\` may have appended
  # — so regeneration is idempotent. strip_modules_block passes a marker-less file
  # through unchanged, so a stock redis.conf is emitted as-is. No banner is
  # prepended: the file simply is your redis.conf with the generated Modules
  # block appended after it (so \`apply\` can copy it to redis.conf verbatim
  # and \`revert\` restores the original by stripping just that block).
  strip_modules_block "$REDIS_CONF"
  cat <<EOF

$MODULES_BEGIN
# Auto-generated by \`make sync-redis-conf\` — do NOT edit this block by hand;
# it is stripped and regenerated on every sync. Everything above is the
# Redis-core config from $REDIS_CONF, emitted verbatim.
#
# Generated:    $generated_at
# Requested:    $requested_display
# Active:       $active_display
# Missing .so:  $missing_display
# Bad manifest: $bad_display

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
        strip_private_block "$conf"
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
