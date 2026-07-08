#!/bin/sh
# run.sh — start src/redis-server with selected modules auto-loaded.
#
# Usage:  scripts/run.sh [<name> ...|all|.|'*'|none]
# Env:    ARGS  extra redis-server args/config (e.g. ARGS="--port 6400")
#
# Tokens:
#   (no args) | all | . | '*'   load every cloned module
#   none                         start with no modules
#   <name> [<name> ...]          load only the named modules
#
# Locates each module's .so via TARGET_MODULE in modules/<name>/Makefile
# (handles redisjson → rejson.so), preferring <host_os>-<arch>-release/.

set -eu

SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)" || exit 1
. "$SCRIPT_DIR/lib/manifest.sh"
cd "$REPO_ROOT"

if [ ! -x src/redis-server ]; then
  echo "ERROR: src/redis-server is not built. Run 'make build' (or 'make -C src all') first."
  exit 1
fi

cloned="$(cloned_modules)"
# "core" is a "no modules" alias — same as "none".
_run_args="$*"
[ "$_run_args" = "core" ] && _run_args="none"
selected="$(resolve_modules "$_run_args" "$cloned" "none")"

# Detect whether the caller named modules explicitly. Wildcards ("", all, ., *)
# and the "none"/"core" tokens resolve without naming specific modules — in that
# case a missing .so is a warning (skip and continue). An explicit name means
# the user expects that module to load — a missing .so is a hard error.
case "${1:-}" in
  ""|all|.|'*'|none|core) explicit=0 ;;
  *) explicit=1 ;;
esac

host_os="$(uname -s | tr '[:upper:]' '[:lower:]')"
[ "$host_os" = "darwin" ] && host_os="macos"

load_flags=""
for name in $selected; do
  # Resolve the .so basename from modules.yaml. Prefer `target_module:`
  # (just-the-artifact), fall back to basename of `loadmodule:`, finally
  # to `<name>.so` so we still try something even if both fields are empty.
  so_base="$(basename "$(manifest_field "$name" target_module)")"
  [ "$so_base" = "." ] || [ -z "$so_base" ] && so_base="$(basename "$(manifest_field "$name" loadmodule)")"
  [ "$so_base" = "." ] || [ -z "$so_base" ] && so_base="$name.so"

  # Each pipeline below ends in `|| true` because grep exits 1 on no match,
  # which combined with `set -euo pipefail` on the parent shell would silently
  # kill the whole script the moment a single un-built module shows up in the
  # loop (the failure escapes the command substitution before the fallback
  # `[ -z "$so_path" ]` chain ever runs). The `WARNING ... skipping` / error
  # branch below is the intended "no .so" handling — keep grep failures out of $?.
  candidates="$(find "modules/$name" -type f -name "$so_base" 2>/dev/null \
      | grep -v -E '/(CMakeFiles|tests?|sample|samples|fixtures)/' || true)"
  so_path="$(echo "$candidates" | grep -E "(^|/)$host_os-[^/]*-release(/|\$)" | head -1 || true)"
  [ -z "$so_path" ] && so_path="$(echo "$candidates" | grep -E '(^|/)release(/|$)' | head -1 || true)"
  [ -z "$so_path" ] && so_path="$(echo "$candidates" | grep -E '(^|/)[^/]*-release(/|$)' | head -1 || true)"
  [ -z "$so_path" ] && so_path="$(echo "$candidates" | head -1 || true)"

  if [ -z "$so_path" ]; then
    if [ "$explicit" = "1" ]; then
      echo "ERROR: $name is not built ($so_base not found under modules/$name)."
      echo "       Run 'make build $name' first."
      exit 1
    fi
    echo "WARNING: no built $so_base found under modules/$name, skipping $name (did you run 'make build $name'?)"
    continue
  fi
  echo "==> Loading $name from $so_path"
  load_flags="$load_flags --loadmodule $so_path"
done

if [ -z "$load_flags" ]; then
  echo "==> No modules selected; starting plain redis-server"
fi

# shellcheck disable=SC2086
echo "==> exec src/redis-server$load_flags ${ARGS:-}"
# shellcheck disable=SC2086
exec src/redis-server $load_flags ${ARGS:-}
