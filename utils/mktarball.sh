#!/bin/sh

if [ "$1" = "" ]
then
    echo "Usage: mktarball.sh <git tag, branch or commit>"
    echo "Example: mktarball.sh 2.2-rc4"
    exit 1
fi

PREFIX="redis-${1}/"
TARBALL="/tmp/redis-${1}.tar.gz"
git archive --format=tar --prefix=$PREFIX $1 | gzip -c > $TARBALL
echo "File created: $TARBALL"
