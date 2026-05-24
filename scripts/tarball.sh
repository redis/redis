#!/usr/bin/env bash
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

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)" || exit 1
. "$SCRIPT_DIR/lib/manifest.sh"
cd "$REPO_ROOT"

MAKE_BIN="${MAKE:-make}"
TAR="${TAR:-$(command -v gtar 2>/dev/null || command -v tar 2>/dev/null)}"

if [ "${TARBALL_SKIP_MODULES_UPDATE:-}" != "1" ]; then
  echo "==> [tarball] pre-step: modules-update all (shallow)"
  MODULES_UPDATE_SHALLOW=1 "$SCRIPT_DIR/modules-update.sh" all
  echo
fi

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
git archive --format=tar "$TAG" | "$TAR" -x -C "$work"

echo
echo "==> Cloning bundled modules per modules.yaml"
for name in $(manifest_modules); do
  repo="$(manifest_field "$name" repo)"
  ref="$(manifest_ref "$name")"
  kind="$(manifest_ref_kind "$name")"
  dest="$work/modules/$name/src"
  mkdir -p "$work/modules/$name"
  if [ -z "$ref" ] || [ -z "$kind" ]; then
    echo "ERROR: '$name' must set one of tag/version/branch/commit in modules.yaml" >&2
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
echo "==> Stripping .git and .github from cloned modules"
find "$work/modules" -name '.git' -prune -exec rm -rf {} + 2>/dev/null || true
find "$work/modules" -type d -name '.github' -prune -exec rm -rf {} + 2>/dev/null || true
find "$work/modules" -name '.gitmodules' -delete 2>/dev/null || true
echo "==> Marking modules pre-prepared (so consumer 'make' skips clone step)"
for name in $(manifest_modules); do
  touch "$work/modules/$name/src/.prepared"
done
if [ -d "$work/modules/redisearch/src" ]; then
  echo "==> Re-creating empty .git marker for redisearch (its build.rs walks up to find one)"
  mkdir -p "$work/modules/redisearch/src/.git"
fi

echo
echo "==> Generating redis-gen.conf in staging (all manifest modules, ASSUME_BUILT=1)"
# Overlay the LOCAL sync-redis-conf script + its lib + the manifest on top
# of whatever came in from `git archive $TAG`, so tarball-time fixes (or
# the modules.yaml-into-modules/ relocation) don't require re-tagging Redis
# core. If $TAG predates that relocation, drop the stale root-level
# modules.yaml so the in-staging tooling has exactly one manifest to find.
mkdir -p "$work/scripts/lib" "$work/modules"
cp scripts/sync-redis-conf.sh "$work/scripts/sync-redis-conf.sh"
cp scripts/lib/manifest.sh    "$work/scripts/lib/manifest.sh"
cp modules/modules.yaml       "$work/modules/modules.yaml"
chmod +x "$work/scripts/sync-redis-conf.sh"
rm -f "$work/modules.yaml"
# Run the script directly against the staging tree (we don't need Make
# here — the script is the contract).
( cd "$work" && ASSUME_BUILT=1 scripts/sync-redis-conf.sh )
echo "==> Promoting redis-gen.conf -> redis.conf, removing redis-gen.conf"
mv "$work/redis-gen.conf" "$work/redis.conf"

echo
echo "==> Producing reproducible tarball at $out"
mtime="$(git log -1 --format=%ct "$TAG")"
if [ -z "$mtime" ]; then
  echo "ERROR: could not resolve commit timestamp for TAG=$TAG" >&2
  exit 1
fi
rm -f "$out"
( cd "$staging" && "$TAR" \
    --sort=name \
    --mtime="@$mtime" \
    --owner=0 --group=0 --numeric-owner \
    --use-compress-program='gzip -n' \
    -cf "$out" "$prefix" )

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
