#!/usr/bin/env bash
#
# Docker image build ONLY — not invoked by `make bootstrap`.
# During `docker build`, runs each entry from `modules/modules.yaml` through
# that module's `src/.install/install_script.sh` (same scripts as a dev
# would run from `make bootstrap` per module).
#
# Environment:
#   ROOT             workspace root (default: /workspace)
#   MODULES_MANIFEST path to modules.yaml (default: $ROOT/modules/modules.yaml)
#   DOCKER_STRICT=1|true|yes  fail the image build if any install_script fails
#
set -euo pipefail

ROOT="${ROOT:-/workspace}"
MANIFEST="${MODULES_MANIFEST:-$ROOT/modules/modules.yaml}"
ME="docker-install-bundled-module-deps.sh"

if [ ! -f "$MANIFEST" ]; then
	echo "$ME: missing manifest: $MANIFEST" >&2
	exit 1
fi

strict_fail() {
	case "${DOCKER_STRICT:-}" in
		1 | true | TRUE | yes | YES) return 0 ;;
		*) return 1 ;;
	esac
}

module_names() {
	awk '
		/^modules:/ { m = 1; next }
		m && /^  - name:/ {
			line = $0
			sub(/^  - name:[[:space:]]*/, "", line)
			sub(/[[:space:]]+#.*$/, "", line)
			gsub(/^[[:space:]]+|[[:space:]]+$/, "", line)
			if (line != "") print line
		}
	' "$MANIFEST"
}

_tmp="$(mktemp)" || { echo "$ME: ERROR: mktemp failed" >&2; exit 1; }
trap 'rm -f "$_tmp"' EXIT
module_names >"$_tmp"
if [ ! -s "$_tmp" ]; then
	echo "$ME: WARNING: no module names parsed from $MANIFEST (expected '  - name:' under 'modules:')" >&2
fi

failed=""
while IFS= read -r name; do
	[ -z "$name" ] && continue
	script="$ROOT/modules/$name/src/.install/install_script.sh"
	if [ ! -f "$script" ]; then
		echo "==> [$name] not cloned (missing $script), skipping"
		continue
	fi
	if [ ! -x "$script" ]; then
		chmod +x "$script" 2>/dev/null || true
	fi
	echo "==> [$name] install_script.sh (OSNICK=${OSNICK:-unknown})"
	script_dir="$(dirname "$script")"
	if ! ( cd "$script_dir" 2>/dev/null ); then
		echo "==> [$name] WARNING: cannot enter $script_dir (continuing)"
		failed="${failed}${failed:+ }${name}"
	elif ! ( cd "$script_dir" && bash "$script" ); then
		echo "==> [$name] WARNING: install_script.sh failed (continuing)"
		failed="${failed}${failed:+ }${name}"
	fi
done <"$_tmp"

if [ -n "$failed" ]; then
	echo "==> WARNING: module installer(s) failed: $failed"
	echo "==> Image still tagged; fix deps or set DOCKER_STRICT=1 (or true/yes) to fail the build."
	if strict_fail; then
		exit 1
	fi
else
	echo "==> All available module installers completed."
fi
