#!/bin/sh
# scripts/lib/manifest.sh — dual-mode YAML manifest reader.
#
# This file is the single source of truth for parsing modules.yaml. It is
# consumed two ways:
#
# 1. As a sourced shell library (the original mode), from other scripts,
#    which must set SCRIPT_DIR (= <repo>/scripts) before sourcing — it
#    seeds REPO_ROOT below:
#
#      SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)"                  # sh
#      SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"   # bash
#      . "$SCRIPT_DIR/lib/manifest.sh"
#      modules="$(manifest_modules)"
#
# 2. As a CLI tool, from Make (see modules/manifest.mk):
#
#      scripts/lib/manifest.sh modules
#      scripts/lib/manifest.sh field <name> <field>
#      scripts/lib/manifest.sh ref <name>
#      scripts/lib/manifest.sh ref-kind <name>
#      scripts/lib/manifest.sh cloned
#      scripts/lib/manifest.sh resolve <requested> <cloned> [extras]
#
# The dispatcher at the bottom of this file is guarded by a $0-basename
# check, so sourcing this file never accidentally runs the CLI; existing
# `. lib/manifest.sh` callers continue to work unchanged.
#
# Both entry points run on bash-less systems: make executes this file via
# its shebang at parse time (modules/manifest.mk), and `make`/`make clean`
# source it from the sh scripts build.sh/clean.sh — busybox ash on the
# alpine:latest Daily CI containers, dash on Debian/Ubuntu. So this file
# must stay POSIX-sh clean (no bashisms). The remaining scripts/*.sh
# callers are bash, which sources POSIX code unchanged.
#
# Shell-side functions exposed when sourced:
#   manifest_modules                           - sorted module names from modules.yaml
#   manifest_field <name> <field>              - one field for one module ("" if missing)
#   manifest_ref <name>                        - the module's `ref:` value verbatim
#   manifest_ref_kind <name>                   - one of: tag | branch | commit
#                                                Resolved against the upstream `repo:`
#                                                via `git ls-remote` in this order:
#                                                  1. tag    — refs/tags/<ref>
#                                                  2. branch — refs/heads/<ref>
#                                                  3. commit — hex SHA (7–40 chars)
#                                                Empty output = no match (bad ref).
#   cloned_modules                             - names with .prepared or .git under modules/<n>/src
#   resolve_modules <requested> <cloned> [allow_extras]
#         Mirrors the case-statement repeated in build/deps/run/test recipes.
#         <allow_extras> is a space-separated list of synonyms in addition to the
#         standard "" / all / . / '*'  (e.g. "redis none" for build, "none" for run).
#         Echoes the resolved space-separated module list. Exits 1 on errors.
#
# YAML format accepted (kept deliberately narrow): a top-level `modules:` key
# followed by list items shaped as
#
#     - name: <module>
#       repo: <url>
#       ref:  <tag | branch | commit>   # required
#
# `ref:` is the single source of pinning; its kind is determined dynamically
# against `repo:`. No nested structures, no inline comments after values, no
# quoted strings.

set -eu

# Executed-vs-sourced detection, POSIX edition: when this file is run
# directly (shebang or `sh scripts/lib/manifest.sh`), $0 is this file's
# path; when sourced, $0 is the caller's (a script path or the shell
# binary). No sourcing caller is named manifest.sh, so the basename is a
# reliable discriminator.
case "${0##*/}" in
  manifest.sh) _MANIFEST_SH_EXECUTED=1 ;;
  *)           _MANIFEST_SH_EXECUTED=  ;;
esac

if [ -n "$_MANIFEST_SH_EXECUTED" ]; then
  # Executed directly: this file lives at <repo>/scripts/lib/manifest.sh.
  REPO_ROOT="${REPO_ROOT:-$(cd -- "$(dirname -- "$0")/../.." && pwd)}"
else
  # Sourced: POSIX sh has no BASH_SOURCE, so the caller must tell us where
  # scripts/ is (every scripts/*.sh caller sets SCRIPT_DIR — see header).
  REPO_ROOT="${REPO_ROOT:-$(cd -- "${SCRIPT_DIR:?manifest.sh: set SCRIPT_DIR before sourcing}/.." && pwd)}"
fi
MODULES_MANIFEST_FILE="${MODULES_MANIFEST_FILE:-$REPO_ROOT/modules/modules.yaml}"

manifest_join_words() {
  awk '
    {
      for (i = 1; i <= NF; i++) {
        if (seen) printf " "
        printf "%s", $i
        seen = 1
      }
    }
    END {
      if (seen) printf "\n"
    }
  '
}

manifest_modules() {
  awk '
    /^[ \t]*-[ \t]*name:[ \t]*/ {
      sub(/^[ \t]*-[ \t]*name:[ \t]*/, "")
      sub(/[ \t]+$/, "")
      if (length() > 0) print
    }
  ' "$MODULES_MANIFEST_FILE" 2>/dev/null | sort -u
}

manifest_field() {
  local want="$1" field="$2"
  awk -v want="$want" -v field="$field" '
    BEGIN { cur = "" }
    /^[ \t]*-[ \t]*name:[ \t]*/ {
      line = $0
      sub(/^[ \t]*-[ \t]*name:[ \t]*/, "", line)
      sub(/[ \t]+$/, "", line)
      cur = line
      next
    }
    cur == want {
      pat = "^[ \t]+" field ":"
      if ($0 ~ pat) {
        v = $0
        sub("^[ \t]+" field ":[ \t]*", "", v)
        sub(/[ \t]+$/, "", v)
        print v
        exit
      }
    }
  ' "$MODULES_MANIFEST_FILE" 2>/dev/null
}

# Read the module's `ref:` value verbatim. Echoes empty if it's not set;
# callers should treat that as an error.
manifest_ref() {
  manifest_field "$1" ref
}

# Determine the kind of `ref:` for a module by probing the upstream `repo:`.
# Priority (first match wins):
#   1. tag    — `refs/tags/<ref>` exists on the remote
#   2. branch — `refs/heads/<ref>` exists on the remote
#   3. commit — `<ref>` is a hex SHA (7–40 chars) and neither of the above
#               matched
# Echoes one of: tag | branch | commit. Empty output means "bad ref" — the
# remote has no such tag or branch and the string does not look like a SHA.
manifest_ref_kind() {
  local name="$1" ref repo
  ref="$(manifest_field "$name" ref)"
  repo="$(manifest_field "$name" repo)"
  [ -z "$ref" ] && return 0
  [ -z "$repo" ] && return 0

  if git ls-remote --tags --exit-code "$repo" "refs/tags/$ref" >/dev/null 2>&1; then
    echo "tag"
    return
  fi
  if git ls-remote --heads --exit-code "$repo" "refs/heads/$ref" >/dev/null 2>&1; then
    echo "branch"
    return
  fi
  if printf '%s' "$ref" | grep -Eq '^[0-9a-f]{7,40}$'; then
    echo "commit"
    return
  fi
}

cloned_modules() {
  local cloned=""
  local name
  for name in $(manifest_modules); do
    if [ -f "$REPO_ROOT/modules/$name/src/.prepared" ] || [ -e "$REPO_ROOT/modules/$name/src/.git" ]; then
      cloned="$cloned $name"
    fi
  done
  printf '%s\n' "$cloned" | manifest_join_words
}

# Echo the resolved module list given user input + the set of cloned modules.
# Special tokens:
#   "" | all | . | '*'   -> the full <cloned> list
# Extras (passed in $3, space-separated) map to "" (empty selection); typical:
#   "redis none" for build, "none" for run.
# Errors to stderr and `exit 1` on unknown / mixed tokens.
resolve_modules() {
  local requested="$1" cloned="$2" extras="${3:-}"

  case "$requested" in
    ""|all|.|'*') echo "$cloned"; return ;;
  esac

  # `all`/`.`/`*` are wildcards and cannot mix with explicit names.
  local r
  for r in $requested; do
    case "$r" in
      all|.|'*')
        echo "ERROR: '$r' cannot be mixed with explicit module names" >&2
        exit 1 ;;
    esac
  done

  # Extra tokens (e.g. `redis`, `none`) act as "no modules selected" — but
  # only when *every* requested token is an extra. Mixing with module names
  # is rejected so we don't silently drop those names.
  if [ -n "$extras" ]; then
    local all_extras=1 r2 e2 matched
    for r2 in $requested; do
      matched=""
      for e2 in $extras; do [ "$r2" = "$e2" ] && matched=1 && break; done
      [ -z "$matched" ] && all_extras="" && break
    done
    if [ -n "$all_extras" ]; then
      echo ""
      return
    fi
    # Otherwise: any token that *is* an extra mixed with names is invalid.
    for r2 in $requested; do
      for e2 in $extras; do
        if [ "$r2" = "$e2" ]; then
          echo "ERROR: '$r2' cannot be mixed with explicit module names" >&2
          exit 1
        fi
      done
    done
  fi

  # Every remaining token must name a cloned module.
  local c found
  for r in $requested; do
    found=""
    for c in $cloned; do [ "$c" = "$r" ] && found=1 && break; done
    if [ -z "$found" ]; then
      echo "ERROR: module '$r' is not available under modules/$r/src" >&2
      echo "  (expect .git or .prepared in modules/$r/src)" >&2
      echo "Modules found: $cloned" >&2
      echo "Hint: run 'make modules-update $r' or clone into modules/$r/src" >&2
      exit 1
    fi
  done
  echo "$requested"
}

# ---------------------------------------------------------------------------
# CLI dispatcher. Only runs when this file is executed directly
# (`scripts/lib/manifest.sh <subcommand>`), not when it's sourced.
# Sourcing detection via the $0 basename — see _MANIFEST_SH_EXECUTED at
# the top of this file.
# ---------------------------------------------------------------------------
if [ -n "$_MANIFEST_SH_EXECUTED" ]; then
  _manifest_usage() {
    cat >&2 <<'USAGE'
Usage: scripts/lib/manifest.sh <subcommand> [args]

Subcommands:
  modules                          Sorted list of module names in modules.yaml.
  field <name> <field>             One field for one module (empty if missing).
  ref <name>                       The module's `ref:` value verbatim.
  ref-kind <name>                  One of: tag | branch | commit, resolved
                                   against the upstream `repo:` via
                                   `git ls-remote` (tag > branch > commit).
  cloned                           Module names with .prepared or .git under
                                   modules/<name>/src.
  resolve <requested> <cloned> [extras]
                                   Apply the build/run/test selection rules.

Also sourceable as a shell library; see the top of this file for the
function-level API.
USAGE
  }

  cmd="${1:-}"
  shift || true
  case "$cmd" in
    modules)
      manifest_modules
      ;;
    field)
      [ $# -eq 2 ] || { echo "ERROR: 'field' takes <name> <field>" >&2; _manifest_usage; exit 2; }
      manifest_field "$1" "$2"
      ;;
    ref)
      [ $# -eq 1 ] || { echo "ERROR: 'ref' takes <name>" >&2; _manifest_usage; exit 2; }
      manifest_ref "$1"
      ;;
    ref-kind)
      [ $# -eq 1 ] || { echo "ERROR: 'ref-kind' takes <name>" >&2; _manifest_usage; exit 2; }
      manifest_ref_kind "$1"
      ;;
    cloned)
      cloned_modules
      ;;
    resolve)
      [ $# -ge 2 ] || { echo "ERROR: 'resolve' takes <requested> <cloned> [extras]" >&2; _manifest_usage; exit 2; }
      resolve_modules "$@"
      ;;
    ""|-h|--help|help)
      _manifest_usage
      [ -z "$cmd" ] && exit 2 || exit 0
      ;;
    *)
      echo "ERROR: unknown subcommand '$cmd'" >&2
      _manifest_usage
      exit 2
      ;;
  esac
fi
