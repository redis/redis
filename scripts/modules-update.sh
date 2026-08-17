#!/bin/sh
# modules-update.sh — clone or refresh modules per modules.yaml.
#
# Usage:  scripts/modules-update.sh [<name> ...|all|.|'*']
#         (no args = all modules in modules.yaml)
#
# Env: MODULES_UPDATE_SHALLOW=1  clone with --depth 1
#      MAKE                      make binary (defaults to `make`)

set -eu

SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)" || exit 1
. "$SCRIPT_DIR/lib/manifest.sh"
cd "$REPO_ROOT"

MAKE_BIN="${MAKE:-make}"

available="$(manifest_modules | manifest_join_words)"
requested="$*"
if [ -z "$requested" ]; then
  echo "==> No module specified — defaulting to all ($available)"
  requested="$available"
fi
for r in $requested; do
  case "$r" in all|.|'*') requested="$available"; break ;; esac
done

# `none` — clone/refresh nothing; just regenerate redis-full.conf with no
# modules, reverting the loadmodule lines / emptying the Modules section (same
# contract as `make sync-redis-conf none`).
for r in $requested; do
  case "$r" in none)
    echo "==> 'none' requested — skipping clone; reverting redis-full.conf module section"
    "$MAKE_BIN" --no-print-directory sync-redis-conf MODULES=none
    exit 0 ;;
  esac
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
  if [ -z "$ref" ]; then
    echo "ERROR: '$name' must set a 'ref' in modules.yaml"; exit 1
  fi
  if [ -z "$kind" ]; then
    echo "ERROR: ref '$ref' for '$name' is neither a tag nor a branch on $repo, and is not a hex commit SHA"; exit 1
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
        current="$(git -C "$dest" rev-parse HEAD)" || {
          echo "ERROR: git rev-parse HEAD failed in $dest" >&2
          exit 1
        }
        # Peel to '^{commit}' so this compares correctly for annotated tags
        # too — FETCH_HEAD for an annotated tag is the tag *object* SHA, not
        # the commit SHA that HEAD (after a normal checkout) actually is;
        # comparing the raw SHAs would never match and the fast path below
        # would silently never trigger for that ref kind.
        target="$(git -C "$dest" rev-parse -q --verify 'FETCH_HEAD^{commit}' 2>/dev/null || true)"
        if [ -n "$target" ] && [ "$current" = "$target" ]; then
          echo "==> $name already at $kind $ref"
        else
          echo "==> Moving $name to $kind $ref"
          # Check out the resolved commit SHA, not "$ref" by name — a plain
          # `git fetch` never updates dest's own local tag/branch ref, so
          # checking out "$ref" here would silently re-select a STALE local
          # ref if one already exists under that name (e.g. a branch's old
          # tip, or a tag force-moved upstream to a new commit) instead of
          # the just-fetched target.
          git -C "$dest" checkout -f "$target" 2>/dev/null \
            || git -C "$dest" reset --hard FETCH_HEAD
        fi
        ;;
    esac
    echo "==> Re-syncing submodules for $name"
    git -C "$dest" submodule sync --recursive
    git -C "$dest" submodule update --init --recursive $depth_args
  fi
  touch "$dest/.prepared"
done

echo
echo "==> Modules updated: $requested"
echo "    Next: run 'make bootstrap [<name> ...]' to install per-module build/test deps."

echo "==> Refreshing redis-full.conf via sync-redis-conf"
# Sync exactly the requested modules — a `make modules-update redistimeseries`
# refreshes that module's loadmodule line/block in redis-full.conf. Each
# module's active/missing state is independently derived from .so presence on
# disk unless ASSUME_BUILT is set.
#
# ASSUME_BUILT=1: emit an active `loadmodule` line for each requested module
# even though this script only clones source and never builds — so a
# `make modules-update redistimeseries` leaves redis-full.conf ready to load
# the module once it's built, instead of a commented "not built" placeholder.
"$MAKE_BIN" --no-print-directory sync-redis-conf MODULES="$requested" ASSUME_BUILT=1
