#!/bin/sh
# deploy.sh — build (via scripts/build.sh) then copy artifacts to $(PREFIX).
#
# Usage:  scripts/deploy.sh [<name> ...|all|.|'*'|redis|none]
# Env:    PREFIX    install root (default /usr/local). Files land in:
#                     $PREFIX/bin/                  - redis-server, -cli, -benchmark
#                     $PREFIX/lib/redis/modules/    - per-module .so files
#         DESTDIR   optional staging root prepended to PREFIX (for packaging).
#         MAKE      make binary (defaults to `make`); only used when shelling
#                   into build.sh, which itself respects it.
#
# Tokens:
#   (no args) | all | . | '*'   build + install Redis + every cloned module
#   redis | none                 Redis only
#   <name> [<name> ...]          Redis + the listed modules
#
# Flow:
#   1. Delegate build to scripts/build.sh — same orchestration the user gets
#      from `make` (failure collection, redis-full.conf refresh, etc.).
#   2. After build, copy artifacts into place ourselves (NOT via each
#      module's `install` target). This keeps the install step a pure copy
#      and avoids depending on per-module Makefile install recipes.
#
# Failures during build are surfaced from build.sh; failures during copy are
# collected and reported at the end of this script.

set -eu

SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)" || exit 1
. "$SCRIPT_DIR/lib/manifest.sh"
cd "$REPO_ROOT"

PREFIX="${PREFIX:-/usr/local}"
DESTDIR="${DESTDIR:-}"

cloned="$(cloned_modules)"
modules="$(resolve_modules "$*" "$cloned" "redis none")"

# ---------------------------------------------------------------------------
# Phase 1: build via the shared orchestrator. Pass the resolved module list
# verbatim so build.sh sees the same selection we intend to install.
# ---------------------------------------------------------------------------
echo "==> Building before deploy (delegating to scripts/build.sh)"
echo
# Build artifacts always go to the dev tree — PREFIX is irrelevant to the
# build phase. Two scrubs needed before handing off to build.sh:
#   1. PREFIX env shadow — so anything reading $PREFIX in build.sh / scripts
#      it spawns sees the empty value.
#   2. MAKEFLAGS — when the user runs `make deploy PREFIX=…`, the parent
#      make smuggles PREFIX through MAKEFLAGS, and any recursive `make` we
#      spawn would re-apply it as a command-line variable (which beats env).
#      Strip just the PREFIX=… token from MAKEFLAGS so the rest (e.g. -j8)
#      still propagates normally.
# Our local $PREFIX (in this script's scope) is untouched and reused for
# the copy + apply-redis-conf phases below.
build_makeflags="$(printf '%s' " ${MAKEFLAGS:-} " | sed -E 's/ PREFIX=[^ ]*/ /g; s/^ *//; s/ *$//')"

# If $modules is empty (`none` token) we still want Redis core built — feed
# the literal "redis" token in that case so build.sh builds core only.
# Tolerate a non-zero exit from build.sh: it returns 1 when any one module
# fails to build, but other modules may have succeeded (or have artifacts left
# over from prior successful builds). The per-module existence check in
# phase 2 below filters out modules that genuinely don't have a .so to copy,
# so we'd rather install what's available than bail out wholesale.
build_rc=0
if [ -z "$modules" ]; then
  MAKEFLAGS="$build_makeflags" PREFIX="" "$SCRIPT_DIR/build.sh" redis || build_rc=$?
else
  MAKEFLAGS="$build_makeflags" PREFIX="" "$SCRIPT_DIR/build.sh" $modules || build_rc=$?
fi
if [ "$build_rc" != 0 ]; then
  echo
  echo "==> WARNING: build.sh reported failures (exit $build_rc) — continuing to install what's available"
fi

# ---------------------------------------------------------------------------
# Phase 2: copy artifacts to $DESTDIR$PREFIX. Pure file ops, no recursive make.
# ---------------------------------------------------------------------------
INSTALL_BIN_DIR="$DESTDIR$PREFIX/bin"
INSTALL_MOD_DIR="$DESTDIR$PREFIX/lib/redis/modules"

echo
echo "==> Deploying to PREFIX=$PREFIX${DESTDIR:+ (DESTDIR=$DESTDIR)}"
echo

# Redis core binaries + the three symlinks that point to redis-server.
echo "==> Installing Redis core binaries to $INSTALL_BIN_DIR"
install -d "$INSTALL_BIN_DIR"
install -m 0755 src/redis-server     "$INSTALL_BIN_DIR/redis-server"
install -m 0755 src/redis-cli        "$INSTALL_BIN_DIR/redis-cli"
install -m 0755 src/redis-benchmark  "$INSTALL_BIN_DIR/redis-benchmark"
ln -sf redis-server "$INSTALL_BIN_DIR/redis-check-rdb"
ln -sf redis-server "$INSTALL_BIN_DIR/redis-check-aof"
ln -sf redis-server "$INSTALL_BIN_DIR/redis-sentinel"

# Per-module `.so` files. After scripts/build.sh, each cloned module has its
# .so copied to modules/<name>/<basename> by common.mk (the `cp $(TARGET_MODULE) ./`
# step in $(TARGET_MODULE)'s recipe). Read the manifest's `target_module:`
# field to know the exact basename for each module (handles redisjson →
# rejson.so) and copy from there.
failed=""
if [ -n "$modules" ]; then
  echo
  echo "==> Installing modules to $INSTALL_MOD_DIR"
  install -d "$INSTALL_MOD_DIR"
  for name in $modules; do
    target="$(manifest_field "$name" target_module)"
    if [ -z "$target" ]; then
      echo "==> [module] $name: no 'target_module' in modules.yaml — skipping"
      failed="$failed $name"
      continue
    fi
    so_basename="$(basename "$target")"
    so_src="modules/$name/$so_basename"
    if [ ! -f "$so_src" ]; then
      echo "==> [module] $name: built artifact not found at $so_src — skipping"
      failed="$failed $name"
      continue
    fi
    install -m 0755 "$so_src" "$INSTALL_MOD_DIR/$so_basename"
    echo "    [module] $name -> $INSTALL_MOD_DIR/$so_basename"
  done
fi

# ---------------------------------------------------------------------------
# Phase 3: rewrite the loadmodule paths in redis-full.conf and redis.conf
# in-place. Only the LOADMODULE_BEGIN/END block is replaced — the rest of
# each file is untouched. Runs even when some modules failed to copy so that
# the successfully-installed ones get correct absolute paths.
# ---------------------------------------------------------------------------
REDIS_FULL_CONF="${REDIS_GEN_CONF:-redis-full.conf}"
REDIS_CONF="${REDIS_CONF:-redis.conf}"
LOADMODULE_BEGIN="# >>> BEGIN: loadmodule paths (replaced by make deploy) <<<"
LOADMODULE_END="# <<< END: loadmodule paths <<<"

# Build the list of successfully-installed modules (i.e. those NOT in $failed).
installed_modules=""
for name in $modules; do
  case " $failed " in *" $name "*) continue ;; esac
  installed_modules="$installed_modules $name"
done
installed_modules="${installed_modules# }"

if [ -n "$installed_modules" ]; then
  new_lines=""
  for name in $installed_modules; do
    target="$(manifest_field "$name" target_module)"
    [ -z "$target" ] && continue
    so_basename="$(basename "$target")"
    new_lines="${new_lines}loadmodule $INSTALL_MOD_DIR/$so_basename
"
  done

  # Write new_lines to a temp file so awk can read it with getline — BSD awk
  # (macOS /usr/bin/awk) rejects embedded newlines in -v values.
  new_lines_file="$(mktemp)"
  printf '%s' "$new_lines" > "$new_lines_file"

  _patch_conf() {
    local conf="$1"
    [ -f "$conf" ] || return 0
    grep -qF "$LOADMODULE_BEGIN" "$conf" 2>/dev/null || return 0
    local tmp
    tmp="$(mktemp "${conf}.deploy.XXXXXX")"
    trap 'rm -f "$tmp" "$new_lines_file"' EXIT
    awk -v begin="$LOADMODULE_BEGIN" -v end="$LOADMODULE_END" -v newfile="$new_lines_file" '
      $0 == begin {
        print
        while ((getline line < newfile) > 0) print line
        close(newfile)
        skip=1; next
      }
      $0 == end   { skip=0 }
      !skip        { print }
    ' "$conf" > "$tmp"
    mv "$tmp" "$conf"
    trap - EXIT
    echo "==> Updated loadmodule paths in $conf"
  }

  _patch_conf "$REDIS_FULL_CONF"
  _patch_conf "$REDIS_CONF"
  rm -f "$new_lines_file"
fi

echo
echo "==> Deploy complete."
echo "    redis-server: $INSTALL_BIN_DIR/redis-server"
if [ -n "$modules" ]; then
  echo "    Module .so directory: $INSTALL_MOD_DIR/"
fi

if [ -n "$failed" ]; then
  echo
  echo "ERROR: deploy finished with module copy failure(s):$failed" >&2
  exit 1
fi
