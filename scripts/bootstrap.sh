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

# Ensure sudo + python3 exist when running as root inside a slim container,
# matching the legacy Makefile recipe behaviour.
if [ "$(uname -s)" = "Linux" ] && [ "$(id -u)" -eq 0 ] && ! command -v sudo >/dev/null 2>&1; then
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

echo "==> Installing deps for: $selected"
export PIP_BREAK_SYSTEM_PACKAGES=1
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
  if ! "$MAKE_BIN" -C "modules/$name/src" bootstrap; then
    failed="$failed $name"
  fi
done

echo
if [ -n "$failed" ]; then
  echo "==> Deps install completed with FAILURES for:$failed"
  echo "    Re-run 'make bootstrap$failed' after fixing the issues above."
  exit 1
fi
echo "==> Deps install complete for: $selected"
echo "    Next: 'make build [<name>]' then 'make test [<name>]' or 'make run'."
