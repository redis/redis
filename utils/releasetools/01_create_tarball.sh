#!/bin/sh
if [ $# != "1" ]
then
    echo "Usage: ./utils/releasetools/01_create_tarball.sh <version_tag>"
    exit 1
fi

TAG=$1
TARNAME="redis-${TAG}.tar"
echo "Generating /tmp/${TARNAME}"
git archive $TAG --prefix redis-${TAG}/ > /tmp/$TARNAME || exit 1
echo "Gizipping the archive"
rm -f /tmp/$TARNAME.gz
gzip -9 /tmp/$TARNAME
