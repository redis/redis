#!/bin/sh
# deploy.sh — build (via scripts/build.sh) then copy artifacts to $(PREFIX).
#
# Usage:  scripts/deploy.sh [<name> ...|all|.|'*'|redis|none]
# Env:    PREFIX    install root (default /usr/local). Files land in:
#                     $PREFIX/bin/                  - redis-server, -cli, -benchmark
#                     $PREFIX/lib/redis/modules/    - per-module .so files
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
# Phase 2: copy artifacts to $PREFIX. Pure file ops, no recursive make.
# ---------------------------------------------------------------------------
INSTALL_BIN_DIR="$PREFIX/bin"
INSTALL_MOD_DIR="$PREFIX/lib/redis/modules"

echo
echo "==> Deploying to PREFIX=$PREFIX"
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
# Phase 3: repoint the loadmodule paths at $INSTALL_MOD_DIR. Path rewrite
# ONLY — inside the LOADMODULE_BEGIN/END block every line keeps its identity
# and its comment state: a commented placeholder stays commented, an active
# line stays active, and no line is ever added or removed. Which modules are
# enabled is sync-redis-conf's call (it tests the .so on disk); deploy must
# not re-author the config just because a module failed to build or wasn't
# part of this run.
# ---------------------------------------------------------------------------
REDIS_FULL_CONF="${REDIS_GEN_CONF:-redis-full.conf}"
REDIS_CONF="${REDIS_CONF:-redis.conf}"
LOADMODULE_BEGIN="# >>> BEGIN: loadmodule paths (replaced by make deploy) <<<"
LOADMODULE_END="# <<< END: loadmodule paths <<<"

_patch_conf() {
  conf="$1"
  [ -f "$conf" ] || return 0
  grep -qF "$LOADMODULE_BEGIN" "$conf" 2>/dev/null || return 0
  tmp="$(mktemp "${conf}.deploy.XXXXXX")"
  # Swap each loadmodule line's directory for $INSTALL_MOD_DIR, keeping the
  # leading '#' (if any), the .so basename and any trailing module args.
  awk -v begin="$LOADMODULE_BEGIN" -v end="$LOADMODULE_END" -v dir="$INSTALL_MOD_DIR" '
    $0 == begin { inblock = 1; print; next }
    $0 == end   { inblock = 0; print; next }
    inblock && match($0, /^[ \t]*#?[ \t]*loadmodule[ \t]+/) {
      head = substr($0, 1, RLENGTH)
      rest = substr($0, RLENGTH + 1)
      sp = index(rest, " ")
      path = (sp ? substr(rest, 1, sp - 1) : rest)
      args = (sp ? substr(rest, sp)        : "")
      n = split(path, parts, "/")
      print head dir "/" parts[n] args
      next
    }
    { print }
  ' "$conf" > "$tmp"
  mv "$tmp" "$conf"
  echo "==> Repointed loadmodule paths in $conf -> $INSTALL_MOD_DIR/"
}

_patch_conf "$REDIS_FULL_CONF"
_patch_conf "$REDIS_CONF"

# ---------------------------------------------------------------------------
# Phase 4: report modules the installed server cannot load — those that
# failed to build / had no artifact to copy, plus manifest modules whose
# source was never cloned. Nothing is edited on their behalf: the user
# comments the matching loadmodule line out (or fixes the build).
# ---------------------------------------------------------------------------
not_cloned=""
for name in $(manifest_modules); do
  case " $cloned " in *" $name "*) continue ;; esac
  case " $failed " in *" $name "*) continue ;; esac   # already reported above
  not_cloned="$not_cloned $name"
done

_report_unloadable() {
  reason="$1"
  shift
  for name in "$@"; do
    target="$(manifest_field "$name" target_module)"
    [ -z "$target" ] && continue
    printf '    %-16s no .so at %s/%s (%s)\n' \
      "$name" "$INSTALL_MOD_DIR" "$(basename "$target")" "$reason"
  done
}

if [ -n "$failed$not_cloned" ]; then
  echo
  echo "==> WARNING: the following modules have no installed .so —"
  echo "    redis-server will abort on their loadmodule line:"
  [ -n "$failed" ]     && _report_unloadable "build failed or artifact missing" $failed
  [ -n "$not_cloned" ] && _report_unloadable "source not cloned" $not_cloned
  echo
  echo "    To start the server without them, comment out their loadmodule"
  echo "    lines in the conf you pass to redis-server, e.g.:"
  echo "      # loadmodule $INSTALL_MOD_DIR/<module>.so"
  echo "    (deploy leaves those lines exactly as they are — it only rewrites"
  echo "     their directory. Fix the build, or run 'make sync-redis-conf'.)"
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
