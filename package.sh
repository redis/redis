SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR=${SCRIPT_DIR}
SRC_DIR="${BUILD_DIR}/src"
REPACK_DIR="${BUILD_DIR}/repack"

XREDIS_NAME=xredis-ror
REDIS_VERSION=$(cat ${SCRIPT_DIR}/src/version.h| grep -w REDIS_VERSION | awk -F \" '{print $2}')
XREDIS_VERSION=$(cat ${SCRIPT_DIR}/src/ctrip.h| grep -w XREDIS_VERSION | awk -F \" '{print $2}')
SWAP_VERSION=$(cat ${SCRIPT_DIR}/src/version.h| grep -w SWAP_VERSION | awk -F \" '{print $2}')
PKG_COMPILE_COUNT="0"
PKG_VERSION="$REDIS_VERSION-$XREDIS_VERSION-$SWAP_VERSION-$PKG_COMPILE_COUNT"

OSRELEASE=""
OSBITS=`uname -p`

ARCH=$(echo `uname -s` | awk '{print toupper($1)}')
if [[ "$ARCH" == "LINUX" ]]; then
    os_release_file=/etc/os-release
    if [[ -s ${os_release_file} ]]; then
        . ${os_release_file}
        OSRELEASE="${ID}${VERSION_ID}"
    else
		echo "/etc/os-release file not found!"
		exit 1
    fi
else
    echo "The system $ARCH does not supported!"
    exit 1
fi

RED='\033[0;31m'
NC='\033[0m' # No Color
cecho() {
    local text="$RED[== $1 ==]$NC" 
    printf "$text\n"
}

# build & install
XREDIS_TAR_NAME=${XREDIS_NAME}"-"${PKG_VERSION}"-"${OSRELEASE}"-"${OSBITS}
XREDIS_TAR_BALL=${XREDIS_TAR_NAME}.tar.gz
XREDIS_INSTALL_DIR=${REPACK_DIR}/${XREDIS_TAR_NAME}

cecho "Build & install xredis to ${XREDIS_INSTALL_DIR}"

cd ${BUILD_DIR} && make distclean
cd ${BUILD_DIR} && make -j >/dev/null

mkdir -p ${XREDIS_INSTALL_DIR}

cp ${SRC_DIR}/{redis-server,redis-benchmark,redis-cli,redis-check-rdb,redis-check-aof,../redis.conf} ${XREDIS_INSTALL_DIR}

#  package
cecho "Package ${XREDIS_TAR_BALL}"
cd ${REPACK_DIR} && tar -czvf ${XREDIS_TAR_BALL} ${XREDIS_TAR_NAME}

