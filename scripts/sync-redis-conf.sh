#!/usr/bin/env bash
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
#   REDIS_GEN_CONF          path to generated conf    (default: redis-gen.conf)
#   MODULES                 space-separated module subset (default: every module
#                           in modules.yaml; pass an explicit list to restrict)
#   ASSUME_BUILT            "1" / "true" / "yes" → emit active loadmodule lines
#                           regardless of whether the .so is present on disk
#                           (used by `make tarball`)
#   MODULES_MANIFEST_FILE   manifest path             (default: modules/modules.yaml)
#
# Output is always rewritten in full and is atomic (write to sibling tmpfile,
# then rename). On any failure the temp file is cleaned up and the existing
# $REDIS_GEN_CONF is left untouched.

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=lib/manifest.sh
. "$SCRIPT_DIR/lib/manifest.sh"
cd "$REPO_ROOT"

REDIS_CONF="${REDIS_CONF:-redis.conf}"
REDIS_GEN_CONF="${REDIS_GEN_CONF:-redis-gen.conf}"

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
AUTO_MGMT_BEGIN="# >>> BEGIN auto-managed modules section <<<"
AUTO_MGMT_END="# <<< END auto-managed modules section <<<"
PRIVATE_BEGIN="# >>> BEGIN redis-gen-conf:private <<<"
PRIVATE_END="# <<< END redis-gen-conf:private <<<"

if [ ! -f "$REDIS_CONF" ]; then
  echo "ERROR: $REDIS_CONF not found" >&2
  exit 1
fi
if [ ! -f "$MODULES_MANIFEST_FILE" ]; then
  echo "ERROR: $MODULES_MANIFEST_FILE not found" >&2
  exit 1
fi

# Resolve the requested module set. Empty MODULES → every manifest module
# (sorted by manifest_modules). An explicit MODULES preserves caller order
# so a deterministic invocation yields a deterministic file.
default_used=0
requested_raw="${MODULES:-}"
if [ -z "$(printf '%s' "$requested_raw" | tr -d '[:space:]')" ]; then
  requested="$(manifest_modules | tr '\n' ' ')"
  default_used=1
else
  requested="$requested_raw"
fi
# Collapse runs of whitespace to single spaces, trim ends.
requested="$(printf '%s\n' "$requested" | xargs || true)"

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
  /^[[:space:]]*-[[:space:]]*name:[[:space:]]*/ {
    line = $0
    sub(/^[[:space:]]*-[[:space:]]*name:[[:space:]]*/, "", line)
    sub(/[[:space:]]+$/, "", line)
    cur = line
    next
  }
  cur != "" && /^[[:space:]]+loadmodule:[[:space:]]*/ {
    v = $0
    sub(/^[[:space:]]+loadmodule:[[:space:]]*/, "", v)
    sub(/[[:space:]]+$/, "", v)
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
  elif [ -f "$so" ] || [ "$ASSUME_BUILT_FLAG" = "1" ]; then
    active="${active}${active:+ }${name}"
  else
    missing="${missing}${missing:+ }${name}"
  fi
done

# Filters reused below. Both implement "skip lines between BEGIN/END
# markers, including the markers themselves". Nesting is NOT supported: a
# nested BEGIN would simply re-arm `skip`, an inner END would re-clear it.
# Files without markers are passed through verbatim, so the filters are
# fully opt-in per module.
strip_block() {
  # strip_block <begin-marker> <end-marker> <file>
  awk -v begin="$1" -v end="$2" '
    $0 == begin { skip = 1; next }
    $0 == end   { skip = 0; next }
    !skip       { print }
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
# tmpfile so a failure mid-emit leaves the existing redis-gen.conf intact.
# (The trap above already covers $LOOKUP_FILE; we extend it here to cover
# the output tmp as well.)
tmp_out="$(mktemp "${REDIS_GEN_CONF}.tmp.XXXXXX")"
trap 'rm -f "$LOOKUP_FILE" "$tmp_out"' EXIT

{
  cat <<EOF
# =============================================================================
# redis-gen.conf — auto-generated by \`make sync-redis-conf\`
# =============================================================================
#
# Untracked, regenerated whenever \`make modules-update\` or \`make build\`
# runs (both call \`sync-redis-conf\` at the end). Two sections:
#
#   1. Redis-core config — verbatim copy of $REDIS_CONF (auto-managed modules
#      block elided). Edit $REDIS_CONF to change Redis-core defaults; the next
#      sync picks them up.
#   2. Modules — only the modules requested by the caller (via the MODULES
#      variable; defaults to all manifest modules when invoked standalone).
#      Each requested module appears as an active \`loadmodule\` + inlined
#      module.conf if its .so is present, or as a commented placeholder
#      otherwise. Modules not in the request list are omitted entirely.
#
# Do NOT edit redis-gen.conf — your edits will be overwritten.
#
# Generated:    $generated_at
# Requested:    $requested_display
# Active:       $active_display
# Missing .so:  $missing_display
# Bad manifest: $bad_display
# =============================================================================

# >>> BEGIN section: Redis-core config (sourced from $REDIS_CONF) <<<

EOF
  strip_block "$AUTO_MGMT_BEGIN" "$AUTO_MGMT_END" "$REDIS_CONF"
  cat <<EOF
# <<< END section: Redis-core config <<<

# >>> BEGIN section: Modules (regenerated on every sync) <<<

EOF

  # `loadmodule` lines (or commented placeholders) come first so the rest of
  # the section is just per-module config blocks.
  for name in $requested; do
    so="$(lookup_so "$name")"
    if [ -z "$so" ]; then
      printf "# %s: 'loadmodule' field missing in modules.yaml\n" "$name"
      continue
    fi
    if [ -f "$so" ] || [ "$ASSUME_BUILT_FLAG" = "1" ]; then
      printf "loadmodule %s\n" "$so"
    else
      printf "# %s: not built (%s absent — run 'make build %s')\n" \
        "$name" "$so" "$name"
      printf "# loadmodule %s\n" "$so"
    fi
  done
  echo

  # Per-module config blocks. Each block is wrapped in its own BEGIN/END
  # markers so it's trivially diff-able and locatable in the generated file.
  for name in $requested; do
    so="$(lookup_so "$name")"
    conf="modules/$name/src/module.conf"
    printf "# >>> BEGIN module: %s <<<\n" "$name"
    if [ -n "$so" ] && { [ -f "$so" ] || [ "$ASSUME_BUILT_FLAG" = "1" ]; }; then
      if [ -f "$conf" ]; then
        strip_block "$PRIVATE_BEGIN" "$PRIVATE_END" "$conf"
      else
        printf "# (no %s found)\n" "$conf"
      fi
    else
      printf "# (module not built — directives elided so redis-server won't\n"
      printf "#  reject unknown config; run 'make build %s' to enable)\n" "$name"
    fi
    printf "# <<< END module: %s <<<\n\n" "$name"
  done

  echo "# <<< END section: Modules <<<"
} > "$tmp_out"

mv "$tmp_out" "$REDIS_GEN_CONF"
trap - EXIT

echo "==> Wrote $REDIS_GEN_CONF"
echo "    requested:    $requested_display"
echo "    active:       $active_display"
echo "    missing .so:  $missing_display"
echo "    bad manifest: $bad_display"
