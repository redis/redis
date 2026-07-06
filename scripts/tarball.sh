#!/bin/sh
# tarball.sh — build a self-contained, reproducible Redis+modules tarball.
#
# Usage:  scripts/tarball.sh
# Required env: TAG=<git-ref>            redis core ref to archive (e.g. 8.0.0)
# Optional env:
#   STAGING_DIR                          default: /tmp/redis-tarball-staging-<tag>
#   OUT_PATH                             default: /tmp/redis-<tag>.tar.gz
#   TAR                                  GNU tar binary (auto-detect: gtar > tar)
#   TARBALL_SKIP_MODULES_UPDATE=1        skip the pre-step modules-update
#
# Produces redis-<tag>/ containing:
#   - redis core source tree (from `git archive <tag>`)
#   - modules/<name>/src/ at the pinned ref from modules.yaml, .git/.github stripped
# Output tar is sorted, with mtimes set to the tag's commit timestamp, owner=0.

set -eu

SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)" || exit 1
. "$SCRIPT_DIR/lib/manifest.sh"
cd "$REPO_ROOT"

MAKE_BIN="${MAKE:-make}"
TAR="${TAR:-$(command -v gtar 2>/dev/null || command -v tar 2>/dev/null)}"

# Validate cheap-to-check inputs BEFORE the expensive modules-update clone, so
# a missing/invalid TAG or unusable tar exits in milliseconds instead of after
# a multi-minute network operation.
if [ -z "${TAG:-}" ]; then
  echo "ERROR: TAG=<tag> is required"
  echo "       e.g. 'make tarball TAG=8.0.0'"
  exit 1
fi
if ! git rev-parse --verify -q "$TAG^{commit}" >/dev/null 2>&1; then
  echo "ERROR: '$TAG' is not a valid git ref in this repo"
  exit 1
fi
if [ -z "$TAR" ]; then
  echo "ERROR: no tar binary found on PATH"; exit 1
fi
if ! "$TAR" --version 2>&1 | grep -qi 'GNU tar'; then
  echo "ERROR: GNU tar required (found: $($TAR --version 2>&1 | head -1))"
  echo "       On macOS: brew install gnu-tar, then retry with TAR=gtar"
  exit 1
fi

if [ "${TARBALL_SKIP_MODULES_UPDATE:-}" != "1" ]; then
  echo "==> [tarball] pre-step: modules-update all (shallow)"
  MODULES_UPDATE_SHALLOW=1 "$SCRIPT_DIR/modules-update.sh" all
  echo
fi

staging="${STAGING_DIR:-/tmp/redis-tarball-staging-$TAG}"
out="${OUT_PATH:-/tmp/redis-$TAG.tar.gz}"
prefix="redis-$TAG"
work="$staging/$prefix"

# Paranoia: refuse to rm -rf any path that isn't clearly a temp/staging dir.
case "$staging" in
  /tmp/*|/var/tmp/*|"$HOME"/.cache/*) ;;
  *) echo "ERROR: refusing to rm -rf STAGING_DIR='$staging' (must be under /tmp, /var/tmp, or \$HOME/.cache)" >&2; exit 1 ;;
esac

echo "==> Staging at $staging"
rm -rf "$staging"
mkdir -p "$work"
echo "==> git archive Redis core @ $TAG → $work"
# Route through a temp file rather than `git archive | tar`: POSIX sh (dash)
# has no `pipefail`, so a piped `git archive` failure would be masked by tar
# exiting 0. The redirect makes a git failure trip `set -e`.
_src_tar="$(mktemp)"
git archive --format=tar "$TAG" > "$_src_tar"
"$TAR" -x -C "$work" -f "$_src_tar"
rm -f "$_src_tar"

echo
echo "==> Cloning bundled modules per modules.yaml"
for name in $(manifest_modules); do
  repo="$(manifest_field "$name" repo)"
  ref="$(manifest_ref "$name")"
  kind="$(manifest_ref_kind "$name")"
  dest="$work/modules/$name/src"
  mkdir -p "$work/modules/$name"
  if [ -z "$ref" ]; then
    echo "ERROR: '$name' must set a 'ref' in modules.yaml" >&2
    exit 1
  fi
  if [ -z "$kind" ]; then
    echo "ERROR: ref '$ref' for '$name' is neither a tag nor a branch on $repo, and is not a hex commit SHA" >&2
    exit 1
  fi
  case "$kind" in
    commit)
      echo "  --> $name @ commit $ref ($repo)"
      git init -q "$dest"
      git -C "$dest" remote add origin "$repo"
      if ! git -C "$dest" fetch --depth 1 origin "$ref" 2>/dev/null; then
        git -C "$dest" fetch origin
      fi
      git -C "$dest" checkout -q --detach "$ref"
      git -C "$dest" submodule update --init --recursive --depth 1
      ;;
    tag|branch)
      echo "  --> $name @ $kind $ref ($repo)"
      git clone --recursive --depth 1 --branch "$ref" "$repo" "$dest"
      ;;
  esac
done

echo
echo "==> Stripping .git, .github, build caches and IDE noise from cloned modules"
find "$work/modules" -name '.git' -prune -exec rm -rf {} + 2>/dev/null || true
find "$work/modules" -type d -name '.github' -prune -exec rm -rf {} + 2>/dev/null || true
find "$work/modules" -name '.gitmodules' -delete 2>/dev/null || true
# Local CMake/Rust build outputs from prior `make` runs. Not in a
# fresh clone, so they have no business in a source tarball. Two-tier
# strip so we don't accidentally remove tracked sources:
#
#   1. Module-root `bin/` and `target/` only — these are the module's
#      own output dirs (never tracked). Per-module explicit path so
#      we don't recurse into `deps/readies/bin/` (vendored scripts,
#      tracked in readies' own repo) or any nested target/ that might
#      be tracked.
#   2. Anywhere — `*-release` / `*-debug` directories. These flavor
#      suffixes only appear on build-output dirs (e.g.
#      `bin/macos-arm64v8-release/`), never on source.
#
# `build/` is NOT in this list — redisearch ships tracked .cmake/Makefile
# recipes under `src/build/{boost,hiredis,libuv}/` that CMakeLists.txt
# `include`s at build time.
for m in $(manifest_modules); do
  [ -d "$work/modules/$m/src/bin" ]    && rm -rf "$work/modules/$m/src/bin"
  [ -d "$work/modules/$m/src/target" ] && rm -rf "$work/modules/$m/src/target"
done
find "$work/modules" -type d \( -name '*-release' -o -name '*-debug' \) \
    -prune -exec rm -rf {} + 2>/dev/null || true
# IDE-local config (Cursor, VSCode, JetBrains). Useless to a tarball consumer.
find "$work/modules" -type d \( -name .cursor -o -name .vscode -o -name .idea \) \
    -prune -exec rm -rf {} + 2>/dev/null || true
# Vendored libraries' OWN benchmark suites and test datasets, found at
# */deps/*/{benchmarks,test_dataset,testdata}. These benchmark/test the
# bundled lib (e.g. fast_double_parser's Gay tests, ScalableVectorSearch's
# fvecs/svs corpora) — the module never references them at build or run
# time. Ship-only-what's-needed: ~35MB saved.
find "$work/modules" -type d \( -name benchmarks -o -name benchmark -o -name test_dataset -o -name testdata \) \
    -path '*/deps/*' -prune -exec rm -rf {} + 2>/dev/null || true
# Module test fixtures — RDB backwards-compat snapshots and large input
# corpora (>1MB CSV/fvecs/svs/txt). Useful only when running the module
# test suite; this is a source/build tarball, not a test-dev kit.
find "$work/modules" -path '*/tests/*' -type f -name '*.rdb' -delete 2>/dev/null || true
find "$work/modules" -path '*/tests/*' -type f \
    \( -name '*.csv' -o -name '*.fvecs' -o -name '*.svs' -o -name '*.txt' \) \
    -size +1M -delete 2>/dev/null || true
echo "==> Marking modules pre-prepared (so consumer 'make build' skips clone step)"
for name in $(manifest_modules); do
  touch "$work/modules/$name/src/.prepared"
done
echo
echo "==> Overlaying local config-sync tooling in staging"
# Overlay the LOCAL sync-redis-conf / apply-redis-conf scripts + their lib
# + the manifest on top of whatever came in from `git archive $TAG`, so
# tarball-time fixes (or the modules.yaml-into-modules/ relocation) don't
# require re-tagging Redis core. If $TAG predates that relocation, drop the
# stale root-level modules.yaml so the in-staging tooling has exactly one
# manifest to find.
mkdir -p "$work/scripts/lib" "$work/modules"
cp scripts/sync-redis-conf.sh   "$work/scripts/sync-redis-conf.sh"
cp scripts/apply-redis-conf.sh  "$work/scripts/apply-redis-conf.sh"
cp scripts/deploy.sh            "$work/scripts/deploy.sh"
cp scripts/lib/manifest.sh      "$work/scripts/lib/manifest.sh"
cp modules/modules.yaml         "$work/modules/modules.yaml"
chmod +x "$work/scripts/sync-redis-conf.sh" "$work/scripts/apply-redis-conf.sh" "$work/scripts/deploy.sh"
rm -f "$work/modules.yaml"

echo
echo "==> Generating redis-full.conf and applying into redis.conf (ASSUME_BUILT=1)"
# Run apply-redis-conf inside the staging tree: generates redis-full.conf with
# relative loadmodule paths, then overwrites redis.conf with the result.
# ASSUME_BUILT=1 emits active loadmodule lines even though .so files aren't
# present yet — consumer builds first, then runs redis-server redis.conf.
( cd "$work" && ASSUME_BUILT=1 bash scripts/apply-redis-conf.sh )

echo
echo "==> Producing reproducible tarball at $out"
mtime="$(git log -1 --format=%ct "$TAG")"
if [ -z "$mtime" ]; then
  echo "ERROR: could not resolve commit timestamp for TAG=$TAG" >&2
  exit 1
fi
rm -f "$out"
# Stage the uncompressed tar to a temp file, then gzip it — same reason as
# above: without `pipefail`, a `tar | gzip` failure in tar would be hidden by
# gzip succeeding. Separating the steps lets `set -e` catch a tar failure.
_pkg_tar="$(mktemp)"
( cd "$staging" && "$TAR" \
    --sort=name \
    --mtime="@$mtime" \
    --owner=0 --group=0 --numeric-owner \
    -c "$prefix" ) > "$_pkg_tar"
gzip -n < "$_pkg_tar" > "$out"
rm -f "$_pkg_tar"

echo "==> Cleaning staging $staging"
rm -rf "$staging"

compute_sha256() {
  local f="$1"
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$f" | awk '{print $1}'
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$f" | awk '{print $1}'
  else
    openssl dgst -sha256 "$f" | awk '{print $NF}'
  fi
}

echo
echo "==> Tarball ready: $out"
size="$(du -h "$out" | awk '{print $1}')"
sha="$(compute_sha256 "$out")"
echo "    size:    $size"
echo "    sha256:  $sha"
