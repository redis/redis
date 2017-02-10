#!/bin/sh
if [ $# != "1" ]
then
    echo "Usage: ./mkrelease.sh <git-ref>"
    exit 1
fi

TAG=$1
TARNAME="redis-${TAG}.tar"
echo "Generating /tmp/${TARNAME}"
cd ~/hack/redis
git archive $TAG --prefix redis-${TAG}/ > /tmp/$TARNAME || exit 1
echo "Gizipping the archive"
rm -f /tmp/$TARNAME.gz
gzip -9 /tmp/$TARNAME
