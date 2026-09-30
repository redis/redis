#!/bin/sh
# bootstrap.sh — install per-module build/test prerequisites.
#
# Usage:  scripts/bootstrap.sh [<name> ...|all|.|'*']
#
# Top-level entry point is `make bootstrap`. Dispatches to each cloned
# module's `make -C modules/<name>/src bootstrap` (the upstream module
# convention).
# Continues past failures, prints a summary, exits non-zero on any failure.
#
# Env: MAKE                       make binary (defaults to `make`)
#      PIP_BREAK_SYSTEM_PACKAGES   forced to 1

set -eu

SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)" || exit 1
. "$SCRIPT_DIR/lib/manifest.sh"
cd "$REPO_ROOT"

MAKE_BIN="${MAKE:-make}"

# list: report which prerequisites are installed vs missing, WITHOUT
# installing. We detect the `list` arg only to drive the skip-guard + unified
# report below — CHECK_DEPS stays INTERNAL to this script and is NOT exported.
# The `list` goal itself is forwarded verbatim to each module's sub-make; each
# module's own Makefile decides what `make bootstrap list` does.
CHECK_DEPS=0
DRY=0
_args=""
for _a in "$@"; do
  case "$_a" in
    list)    CHECK_DEPS=1 ;;
    dry-run) DRY=1 ;;
    *) _args="$_args $_a" ;;
  esac
done
# shellcheck disable=SC2086
set -- $_args

# dry-run headline lines are cyan — distinct from the blue command lines the
# modules print (plain when piped, e.g. CI logs).
if [ "$DRY" = 1 ] && [ -t 1 ]; then _DB="$(printf '\033[1;36m')"; _DR="$(printf '\033[0m')"; else _DB=""; _DR=""; fi

# Ensure sudo + python3 exist when running as root inside a slim container,
# matching the legacy Makefile recipe behaviour.
if [ "$CHECK_DEPS" != 1 ] && [ "$DRY" != 1 ] && [ "$(uname -s)" = "Linux" ] && [ "$(id -u)" -eq 0 ] && ! command -v sudo >/dev/null 2>&1; then
  if command -v apt-get >/dev/null 2>&1; then
    apt-get update -qq && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends sudo python3
  elif command -v dnf >/dev/null 2>&1; then
    dnf install -y sudo python3
  elif command -v tdnf >/dev/null 2>&1; then
    tdnf install -y sudo python3
  elif command -v apk >/dev/null 2>&1; then
    apk add --no-cache bash make git sudo python3
  fi
fi

cloned="$(cloned_modules)"
selected="$(resolve_modules "$*" "$cloned")"

if [ -z "$selected" ]; then
  echo "ERROR: no modules to install deps for (no modules/*/src with .git or .prepared)"
  echo "       run 'make modules-update all' or clone into modules/<name>/src"
  exit 1
fi

if [ "$CHECK_DEPS" = 1 ]; then
  echo "==> Checking deps for: $selected (no installation)"
elif [ "$DRY" = 1 ]; then
  echo "${_DB}==> Dry-run for: $selected (printing install commands for missing deps, no installation)${_DR}"
else
  echo "==> Installing deps for: $selected"
fi
export PIP_BREAK_SYSTEM_PACKAGES=1

# list: modules append "ok|missing <pkg>" records here instead of each
# printing its own list; we print one deduped union across all modules below.
if [ "$CHECK_DEPS" = 1 ]; then
  DEPS_REPORT_FILE="$(mktemp)"
  export DEPS_REPORT_FILE
  trap 'rm -f "$DEPS_REPORT_FILE"' EXIT
fi

failed=""
for name in $selected; do
  echo
  echo "==> [deps] $name"
  src_mk="modules/$name/src/Makefile"
  if [ ! -f "$src_mk" ]; then
    echo "    !! SKIP: $src_mk does not exist"
    echo "       (the upstream clone may be incomplete; try 'make modules-update $name')"
    failed="$failed $name"
    continue
  fi
  # Contract: any module that defines a `bootstrap` target MUST honor the
  # read-only `list` and `dry-run` goals too (record deps / print commands,
  # install nothing). We forward the goal verbatim and trust that — no
  # capability probe. A module that violates the contract is its own bug.
  # Per-module convention: the inner target is still called `bootstrap` —
  # that's defined by each module's own Makefile, not by us.
  if ! grep -qE '^bootstrap[[:space:]]*:' "$src_mk"; then
    echo "    !! SKIP: no 'bootstrap' target in $src_mk"
    echo "       Add one to the upstream Makefile, e.g.:"
    echo "           bootstrap:"
    echo "                   ./sbin/setup"
    echo "           .PHONY: bootstrap"
    echo "       then commit & push to the module's repo."
    failed="$failed $name"
    continue
  fi
  # Forward the goal verbatim; the module's Makefile interprets it.
  if [ "$CHECK_DEPS" = 1 ]; then _goal="bootstrap list"
  elif [ "$DRY" = 1 ]; then _goal="bootstrap dry-run"
  else _goal="bootstrap"; fi
  # shellcheck disable=SC2086
  if ! "$MAKE_BIN" -C "modules/$name/src" $_goal; then
    failed="$failed $name"
  fi
done

echo
# A structural skip (missing clone/Makefile or no 'bootstrap' target) already
# printed "!! SKIP" inline above. In list mode the module only records (always
# exits 0), so a non-zero $failed here is such a skip — it must NOT preempt the
# deduped union summary below. Only install/dry-run failures short-circuit.
if [ -n "$failed" ] && [ "$CHECK_DEPS" != 1 ]; then
  if [ "$DRY" = 1 ]; then
    echo "==> Dry-run errored for:$failed (see output above)"
  else
    echo "==> Deps install completed with FAILURES for:$failed"
    echo "    Re-run 'make bootstrap$failed' after fixing the issues above."
  fi
  exit 1
fi
if [ "$CHECK_DEPS" = 1 ]; then
  # One deduped union across every checked module. A package's installed state
  # is host-global, so dedup by name is safe (no per-module conflicts).
  if [ ! -s "$DEPS_REPORT_FILE" ]; then
    # Nothing landed in the report: every selected module was skipped
    # (unsupported) or structurally broken — the check verified nothing, so
    # this is a failure, not a green "all clear".
    echo "==> Deps check: nothing verified (no selected module supports 'list')"
    exit 1
  fi
  # Missing records are "pkg" or "pkg:minversion" (a present-but-too-old dep).
  # Dedup to package names; the required version (if any) is resolved per pkg
  # below as the MAX across modules (strictest floor wins).
  mtokens=$(awk '$1=="missing"{print $2}' "$DEPS_REPORT_FILE" | sort -u)
  missing=$(printf '%s\n' "$mtokens" | sed 's/:.*//' | sort -u | sed '/^$/d')
  # "installed" = REQUIRED deps present (ok records only) so the n_ok/total ratio
  # below stays a clean "required installed / required total". $present also
  # includes opt_ok (optional present) — used only to reconcile opt_missing.
  installed=$(awk '$1=="ok"{print $2}' "$DEPS_REPORT_FILE" | sort -u)
  present=$(awk '$1=="ok"||$1=="opt_ok"{print $2}' "$DEPS_REPORT_FILE" | sort -u)
  opt_missing=$(awk '$1=="opt_missing"{print $2}' "$DEPS_REPORT_FILE" | sort -u)
  # Required wins: a package required-missing anywhere is dropped from installed
  # and from the optional list.
  if [ -n "$missing" ]; then
    [ -n "$installed" ]   && installed=$(printf '%s\n' "$installed" | grep -vxF "$missing" || true)
    [ -n "$opt_missing" ] && opt_missing=$(printf '%s\n' "$opt_missing" | grep -vxF "$missing" || true)
  fi
  # Present wins: a package present anywhere (ok OR opt_ok) can't also be
  # "optional, not installed" — drop it from opt_missing so the two lists are
  # disjoint.
  [ -n "$opt_missing" ] && [ -n "$present" ] && opt_missing=$(printf '%s\n' "$opt_missing" | grep -vxF "$present" || true)
  if [ -z "$missing" ];   then n_missing=0; else n_missing=$(printf '%s\n' "$missing" | sed '/^$/d' | wc -l | tr -d ' '); fi
  if [ -z "$installed" ]; then n_ok=0;      else n_ok=$(printf '%s\n' "$installed" | sed '/^$/d' | wc -l | tr -d ' '); fi
  total=$((n_ok + n_missing))
  if [ -t 1 ]; then RED="$(printf '\033[1;31m')"; GRN="$(printf '\033[1;32m')"; YLW="$(printf '\033[1;33m')"; RST="$(printf '\033[0m')"; else RED=""; GRN=""; YLW=""; RST=""; fi
  echo "==> Dependency check across: $selected — nothing installed"
  if [ "$n_missing" -gt 0 ]; then
    echo "${RED}NOT INSTALLED ($n_missing):${RST}"
    printf '%s\n' "$missing" | while IFS= read -r p; do
      [ -n "$p" ] || continue
      need=$(printf '%s\n' "$mtokens" | awk -F: -v pp="$p" '$1==pp && NF>1{print $2}' | sort -t. -k1,1n -k2,2n -k3,3n -k4,4n | tail -1)
      if [ -n "$need" ]; then echo "${RED}    $p (>= $need)${RST}"; else echo "${RED}    $p${RST}"; fi
    done
  else
    echo "${GRN}not installed: (none)${RST}"
  fi
  if [ -n "$opt_missing" ]; then
    echo "${YLW}OPTIONAL, not installed (tests/coverage/debug — won't fail the check):${RST}"
    printf '%s\n' "$opt_missing" | while IFS= read -r p; do [ -n "$p" ] && echo "${YLW}    $p${RST}"; done
  fi
  if [ "${VERBOSE:-0}" = 1 ]; then
    echo "${GRN}installed:${RST}"
    printf '%s\n' "$installed" | while IFS= read -r p; do [ -n "$p" ] && echo "${GRN}    $p${RST}"; done
  else
    echo "${GRN}installed: $n_ok/$total (set VERBOSE=1 to list)${RST}"
  fi
  echo "    Run 'make bootstrap' to install anything not installed."
  # A structural skip (printed inline as "!! SKIP" above) still fails the check.
  { [ "$n_missing" -eq 0 ] && [ -z "$failed" ]; } || exit 1
elif [ "$DRY" = 1 ]; then
  echo "${_DB}==> Dry-run complete for: $selected (commands above are what bootstrap would run; nothing installed)${_DR}"
else
  echo "==> Deps install complete for: $selected"
  echo "    Next: 'make build [<name>]' then 'make test [<name>]' or 'make run'."
fi
