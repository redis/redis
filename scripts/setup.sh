#!/usr/bin/env bash
# setup.sh — modules-update + bootstrap in one shot.
#
# Usage:  scripts/setup.sh [<name> ...|all|.|'*']
# Env:    MAKE  make binary (defaults to `make`)

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)" || exit 1
. "$SCRIPT_DIR/lib/manifest.sh"
cd "$REPO_ROOT"

MAKE_BIN="${MAKE:-make}"
requested="$*"
if [ -z "$requested" ]; then
  available="$(manifest_modules | xargs)"
  echo "==> No module specified — defaulting to all ($available)"
  requested="$available"
fi

echo "==> [setup] Step 1/2: modules-update $requested"
"$MAKE_BIN" --no-print-directory modules-update $requested
echo
echo "==> [setup] Step 2/2: bootstrap $requested"
"$MAKE_BIN" --no-print-directory bootstrap $requested
