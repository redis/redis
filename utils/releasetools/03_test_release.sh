#!/bin/sh
if [ $# != "1" ]
then
    echo "Usage: ${0} <git-ref>"
    exit 1
fi

TAG=$1
TARNAME="redis-${TAG}.tar.gz"
DOWNLOADURL="http://download.redis.io/releases/${TARNAME}"

ssh antirez@metal "export TERM=xterm;
                   cd /tmp;
                   rm -rf test_release_tmp_dir;
                   cd test_release_tmp_dir;
                   rm -f $TARNAME;
                   rm -rf redis-${TAG};
                   wget $DOWNLOADURL;
                   tar xvzf $TARNAME;
                   cd redis-${TAG};
                   make;
                   ./runtest;
                   ./runtest-sentinel;
                   if [ -x runtest-cluster ]; then
                       ./runtest-cluster;
                   fi"
