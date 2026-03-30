#!/bin/sh
#
# Create a Redis source tarball.
#
# Usage:
#   ./utils/releasetools/01_create_tarball.sh <version_tag> [--with-modules]
#
# --with-modules: Include all module sources from modules/*/src and generate
#                 a flattened redis-full.conf (renamed to redis.conf in the
#                 tarball) so the tarball is buildable without Git.

set -eu

if [ $# -lt 1 ]; then
    echo "Usage: ./utils/releasetools/01_create_tarball.sh <version_tag> [--with-modules]"
    exit 1
fi

TAG="$1"
WITH_MODULES=0
if [ "${2:-}" = "--with-modules" ]; then
    WITH_MODULES=1
fi

TARPREFIX="redis-${TAG}"
OUT_TAR="/tmp/${TARPREFIX}.tar"

if [ "${WITH_MODULES}" -eq 0 ]; then
    echo "Generating ${OUT_TAR}.gz"
    git archive "${TAG}" --prefix "${TARPREFIX}/" > "${OUT_TAR}" || exit 1
    echo "Gzipping the archive"
    rm -f "${OUT_TAR}.gz"
    gzip -9 "${OUT_TAR}"
else
    echo "Generating ${OUT_TAR}.gz (including modules)"

    STAGE_DIR="$(mktemp -d "/tmp/${TARPREFIX}.stage.XXXXXX")"
    trap 'rm -rf "${STAGE_DIR}"' EXIT INT TERM

    mkdir -p "${STAGE_DIR}/${TARPREFIX}"

    # Export the superproject at the requested tag
    git archive "${TAG}" --prefix "${TARPREFIX}/" | tar -xf - -C "${STAGE_DIR}"

    # Copy each module's source tree into the staged tarball
    MANIFEST="modules.json"
    if [ ! -f "${MANIFEST}" ]; then
        echo "ERROR: modules.json not found" >&2
        exit 1
    fi

    MODULES=$(python3 -c "
import json
with open('${MANIFEST}') as f:
    data = json.load(f)
for name in data['modules']:
    print(name)
")

    for MODULE_NAME in ${MODULES}; do
        SRC_DIR="modules/${MODULE_NAME}/src"
        if [ ! -d "${SRC_DIR}" ]; then
            echo "WARNING: Module source not found at ${SRC_DIR}, skipping" >&2
            continue
        fi

        echo "  Including module: ${MODULE_NAME}"
        DEST="${STAGE_DIR}/${TARPREFIX}/modules/${MODULE_NAME}/src"
        mkdir -p "${DEST}"

        # Use git checkout-index if inside a git tree, otherwise plain copy
        if (cd "${SRC_DIR}" && git rev-parse --is-inside-work-tree >/dev/null 2>&1); then
            (cd "${SRC_DIR}" && git checkout-index -a -f --prefix="${DEST}/")
            # Also export nested submodules
            (cd "${SRC_DIR}" && git submodule status --recursive 2>/dev/null | awk '{print $2}') | while read -r SM_PATH; do
                if [ -n "${SM_PATH}" ] && [ -d "${SRC_DIR}/${SM_PATH}" ]; then
                    mkdir -p "${DEST}/${SM_PATH}"
                    (cd "${SRC_DIR}/${SM_PATH}" && git checkout-index -a -f --prefix="${DEST}/${SM_PATH}/")
                fi
            done
        else
            cp -a "${SRC_DIR}/." "${DEST}/"
        fi
    done

    # Generate redis-full.conf inside the tarball and use it as the default redis.conf
    echo "  Generating redis-full.conf for tarball"
    FULL_CONF="${STAGE_DIR}/${TARPREFIX}/redis-full.conf"

    # Keep the original redis.conf as redis-minimal.conf
    cp "${STAGE_DIR}/${TARPREFIX}/redis.conf" "${STAGE_DIR}/${TARPREFIX}/redis-minimal.conf"

    # Build redis-full.conf: base config + module loadmodule + module configs
    cp "${STAGE_DIR}/${TARPREFIX}/redis.conf" "${FULL_CONF}"
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
            echo "# loadmodule for ${MODULE_NAME} — path depends on build platform"
            echo "# After building, the .so will be at: modules/${MODULE_NAME}/src/bin/<os>-<arch>-release/${SO_NAME}"
        done

        for MODULE_NAME in ${MODULES}; do
            MODULE_CONF="${STAGE_DIR}/${TARPREFIX}/modules/${MODULE_NAME}/src/module.conf"
            if [ -f "${MODULE_CONF}" ]; then
                CONF_CONTENT=$(cat "${MODULE_CONF}")
                if [ -n "${CONF_CONTENT}" ] && echo "${CONF_CONTENT}" | grep -qv '^[[:space:]]*$'; then
                    echo ""
                    cat "${MODULE_CONF}"
                fi
            fi
        done
        echo ""
    } >> "${FULL_CONF}"

    # In the tarball, redis-full.conf becomes the default redis.conf
    cp "${FULL_CONF}" "${STAGE_DIR}/${TARPREFIX}/redis.conf"

    # Create the tarball
    tar -cf "${OUT_TAR}" -C "${STAGE_DIR}" "${TARPREFIX}"
    echo "Gzipping the archive"
    rm -f "${OUT_TAR}.gz"
    gzip -9 "${OUT_TAR}"
fi

echo "Done: /tmp/${TARPREFIX}.tar.gz"
