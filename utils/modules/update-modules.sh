#!/bin/sh
# update-modules.sh — Clone or update Redis module repositories and generate
# a unified redis-full.conf.
#
# Usage:
#   utils/modules/update-modules.sh [--skip-deps] [--only <name>]
#
# Options:
#   --skip-deps   Skip dependency installation (for packaging / air-gapped environments)
#   --only <name> Only process the named module (e.g. --only redisjson)
#
# The script reads modules.json from the repository root and for each module:
#   1. Clones or updates the repository into modules/<name>/src
#   2. Checks out the pinned ref (tag / branch / SHA)
#   3. Recursively initializes submodules
#   4. (optional) Fetches Rust crate dependencies for Rust-based modules
#
# After all modules are processed it generates redis-full.conf by concatenating
# redis.conf with each module's module.conf (flattened, no 'include' directives).

set -eu

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
MANIFEST="${ROOT_DIR}/modules.json"
MODULES_DIR="${ROOT_DIR}/modules"

SKIP_DEPS=0
ONLY_MODULE=""

while [ $# -gt 0 ]; do
    case "$1" in
        --skip-deps) SKIP_DEPS=1; shift ;;
        --only) ONLY_MODULE="$2"; shift 2 ;;
        *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
done

if [ ! -f "${MANIFEST}" ]; then
    echo "ERROR: modules.json not found at ${MANIFEST}" >&2
    exit 1
fi

if ! command -v python3 >/dev/null 2>&1; then
    echo "ERROR: python3 is required to parse modules.json" >&2
    exit 1
fi

MODULES=$(python3 -c "
import json, sys
with open('${MANIFEST}') as f:
    data = json.load(f)
for name in data['modules']:
    print(name)
")

FAILED=""
PROCESSED=0

for MODULE_NAME in ${MODULES}; do
    if [ -n "${ONLY_MODULE}" ] && [ "${MODULE_NAME}" != "${ONLY_MODULE}" ]; then
        continue
    fi

    MODULE_META=$(python3 -c "
import json
with open('${MANIFEST}') as f:
    data = json.load(f)
m = data['modules']['${MODULE_NAME}']
print(m['repo'])
print(m['ref'])
print(m.get('uses_rust', False))
")

    REPO=$(echo "${MODULE_META}" | sed -n '1p')
    REF=$(echo "${MODULE_META}" | sed -n '2p')
    USES_RUST=$(echo "${MODULE_META}" | sed -n '3p')

    SRC_DIR="${MODULES_DIR}/${MODULE_NAME}/src"

    echo ""
    echo "=== ${MODULE_NAME} ==="
    echo "  repo: ${REPO}"
    echo "  ref:  ${REF}"

    if [ -d "${SRC_DIR}/.git" ]; then
        echo "  Updating existing clone..."
        cd "${SRC_DIR}"
        git fetch --tags --force origin 2>/dev/null || true
        git checkout "${REF}" 2>/dev/null || git checkout "origin/${REF}" 2>/dev/null || {
            echo "  ERROR: Could not checkout ref '${REF}'" >&2
            FAILED="${FAILED} ${MODULE_NAME}"
            cd "${ROOT_DIR}"
            continue
        }
        git submodule sync --recursive >/dev/null 2>&1 || true
        git submodule update --init --recursive
        cd "${ROOT_DIR}"
    else
        echo "  Cloning..."
        rm -rf "${SRC_DIR}"
        mkdir -p "${SRC_DIR}"
        git clone --recursive "${REPO}" "${SRC_DIR}" || {
            echo "  ERROR: Clone failed for ${MODULE_NAME}" >&2
            FAILED="${FAILED} ${MODULE_NAME}"
            continue
        }
        cd "${SRC_DIR}"
        git checkout "${REF}" 2>/dev/null || git checkout "origin/${REF}" 2>/dev/null || {
            echo "  ERROR: Could not checkout ref '${REF}'" >&2
            FAILED="${FAILED} ${MODULE_NAME}"
            cd "${ROOT_DIR}"
            continue
        }
        git submodule update --init --recursive
        cd "${ROOT_DIR}"
    fi

    if [ "${SKIP_DEPS}" -eq 0 ] && [ "${USES_RUST}" = "True" ]; then
        echo "  Fetching Rust crate dependencies..."
        (cd "${SRC_DIR}" && cargo fetch) || {
            echo "  WARNING: cargo fetch failed for ${MODULE_NAME}" >&2
        }
    fi

    PROCESSED=$((PROCESSED + 1))
    echo "  Done."
done

# Generate redis-full.conf (flattened, no include directives)
echo ""
echo "=== Generating redis-full.conf ==="

FULL_CONF="${ROOT_DIR}/redis-full.conf"

# Start with the base redis.conf
cp "${ROOT_DIR}/redis.conf" "${FULL_CONF}"

# Append loadmodule directives and module configs
{
    echo ""
    echo "################################## MODULES ####################################"
    echo ""

    for MODULE_NAME in ${MODULES}; do
        SO_NAME=$(python3 -c "
import json
with open('${MANIFEST}') as f:
    data = json.load(f)
print(data['modules']['${MODULE_NAME}']['so_name'])
")
        SO_PATH=$(find "${MODULES_DIR}/${MODULE_NAME}" -name "${SO_NAME}" 2>/dev/null | head -1)
        if [ -n "${SO_PATH}" ]; then
            SO_REL=$(python3 -c "import os; print(os.path.relpath('${SO_PATH}', '${ROOT_DIR}'))")
            echo "loadmodule ./${SO_REL}"
        else
            echo "# loadmodule for ${MODULE_NAME} (build first, then re-run to update path)"
        fi
    done

    # Append each module's config (if it exists)
    for MODULE_NAME in ${MODULES}; do
        MODULE_CONF="${MODULES_DIR}/${MODULE_NAME}/src/module.conf"
        if [ -f "${MODULE_CONF}" ]; then
            CONF_CONTENT=$(cat "${MODULE_CONF}")
            if [ -n "${CONF_CONTENT}" ] && echo "${CONF_CONTENT}" | grep -qv '^[[:space:]]*$'; then
                echo ""
                cat "${MODULE_CONF}"
            fi
        fi
    done

    echo ""
    echo "################################## SECURITY ###################################"
    echo "#"
    echo "# The following is a list of command categories and their meanings:"
    echo "#"
    echo "# * search - Query engine related."
    echo "# * json - Data type: JSON related."
    echo "# * timeseries -  Data type: time series related."
    echo "# * bloom - Data type:  Bloom filter related."
    echo "# * cuckoo - Data type: cuckoo filter related."
    echo "# * topk -  Data type: top-k related."
    echo "# * cms - Data type: count-min sketch related."
    echo "# * tdigest -  Data type: t-digest related."
    echo ""
} >> "${FULL_CONF}"

echo "  Generated: redis-full.conf"

echo ""
echo "=== Summary ==="
echo "  Modules processed: ${PROCESSED}"
if [ -n "${FAILED}" ]; then
    echo "  FAILED:${FAILED}"
    exit 1
fi
echo "  All modules updated successfully."
