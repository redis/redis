#!/usr/bin/env bash
# modules-update.sh — clone or refresh modules per modules.yaml.
#
# Usage:  scripts/modules-update.sh [<name> ...|all|.|'*']
#         (no args = all modules in modules.yaml)
#
# Env: MODULES_UPDATE_SHALLOW=1  clone with --depth 1
#      MAKE                      make binary (defaults to `make`)

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)" || exit 1
. "$SCRIPT_DIR/lib/manifest.sh"
cd "$REPO_ROOT"

MAKE_BIN="${MAKE:-make}"

available="$(manifest_modules | xargs)"
requested="$*"
if [ -z "$requested" ]; then
  echo "==> No module specified — defaulting to all ($available)"
  requested="$available"
fi
for r in $requested; do
  case "$r" in all|.|'*') requested="$available"; break ;; esac
done

depth_args=""
if [ "${MODULES_UPDATE_SHALLOW:-}" = "1" ]; then
  echo "==> MODULES_UPDATE_SHALLOW=1: cloning with --depth 1"
  depth_args="--depth 1"
fi

for name in $requested; do
  case " $available " in *" $name "*) ;; *)
    echo "ERROR: unknown module '$name' (not listed in modules.yaml)"
    echo "Available modules: $available"
    exit 1 ;;
  esac

  repo="$(manifest_field "$name" repo)"
  ref="$(manifest_ref "$name")"
  kind="$(manifest_ref_kind "$name")"
  dest="modules/$name/src"

  if [ -z "$repo" ]; then
    echo "ERROR: 'repo' is not set for '$name' in modules.yaml"; exit 1
  fi
  if [ -z "$ref" ] || [ -z "$kind" ]; then
    echo "ERROR: '$name' must set one of tag/version/branch/commit in modules.yaml"; exit 1
  fi

  if [ ! -d "$dest/.git" ]; then
    rm -rf "$dest"
    case "$kind" in
      tag|branch)
        echo "==> Cloning $name @ $kind $ref from $repo into $dest"
        git clone --recursive $depth_args --branch "$ref" "$repo" "$dest"
        ;;
      commit)
        echo "==> Cloning $name @ commit $ref from $repo into $dest"
        git init -q "$dest"
        git -C "$dest" remote add origin "$repo"
        if [ -n "$depth_args" ]; then
          git -C "$dest" fetch $depth_args origin "$ref" 2>/dev/null \
            || { echo "    (shallow SHA fetch not supported by server, doing full fetch)"; \
                 git -C "$dest" fetch origin; }
        else
          git -C "$dest" fetch origin
        fi
        git -C "$dest" checkout -q --detach "$ref"
        git -C "$dest" submodule update --init --recursive $depth_args
        ;;
    esac
  else
    case "$kind" in
      commit)
        current="$(git -C "$dest" rev-parse HEAD)" || {
          echo "ERROR: git rev-parse HEAD failed in $dest" >&2
          exit 1
        }
        if [ -z "$current" ]; then
          echo "ERROR: empty HEAD in $dest" >&2
          exit 1
        fi
        if [ "$current" = "$ref" ] || [ "${current#$ref}" != "$current" ]; then
          echo "==> $name already at commit $ref"
        else
          echo "==> Moving $name to commit $ref"
          if [ -n "$depth_args" ]; then
            git -C "$dest" fetch $depth_args origin "$ref" 2>/dev/null \
              || { echo "    (shallow SHA fetch not supported by server, doing full fetch)"; \
                   git -C "$dest" fetch origin; }
          else
            git -C "$dest" fetch origin "$ref" 2>/dev/null \
              || git -C "$dest" fetch origin
          fi
          git -C "$dest" checkout -f --detach "$ref"
        fi
        ;;
      tag|branch)
        echo "==> Ensuring $name is at $kind $ref"
        git -C "$dest" fetch $depth_args origin "$ref" 2>/dev/null \
          || git -C "$dest" fetch $depth_args origin "refs/tags/$ref:refs/tags/$ref" 2>/dev/null \
          || git -C "$dest" fetch origin
        git -C "$dest" checkout -f "$ref" 2>/dev/null \
          || git -C "$dest" reset --hard FETCH_HEAD
        ;;
    esac
    echo "==> Re-syncing submodules for $name"
    git -C "$dest" submodule sync --recursive
    git -C "$dest" submodule update --init --recursive $depth_args
  fi
  touch "$dest/.prepared"
done

echo
echo "==> Refreshing redis-gen.conf via sync-redis-conf"
"$MAKE_BIN" --no-print-directory sync-redis-conf MODULES="$requested"

echo
echo "==> Modules updated: $requested"
echo "    Next: run 'make bootstrap [<name> ...]' to install per-module build/test deps."
