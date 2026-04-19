#!/bin/bash
if [ $# != "2" ]
then
    echo "Usage: ./utils/releasetools/02_upload_tarball.sh <version_tag> <--stable|--unstable>"
    exit 1
fi

VERSION_TAG=$1
VERSION_TYPE=$2

# Validate the stability flag
if [ "$VERSION_TYPE" != "--stable" ] && [ "$VERSION_TYPE" != "--unstable" ]
then
    echo "Error: Second parameter must be either --stable or --unstable"
    echo "Usage: ./utils/releasetools/02_upload_tarball.sh <version_tag> <--stable|--unstable>"
    exit 1
fi

echo "Uploading..."
scp /tmp/redis-${VERSION_TAG}.tar.gz ubuntu@host.redis.io:/var/www/download/releases/

# Only update the stable version if --stable flag is provided
if [ "$VERSION_TYPE" == "--stable" ]
then
    echo "Updating web site... "
    echo "Please check the github action tests for the release."
    echo "Press any key if it is a stable release, or Ctrl+C to abort"
    read x
    ssh ubuntu@host.redis.io "cd /var/www/download;
                              rm -rf redis-${VERSION_TAG}.tar.gz;
                              wget http://download.redis.io/releases/redis-${VERSION_TAG}.tar.gz;
                              tar xvzf redis-${VERSION_TAG}.tar.gz;
                              rm -rf redis-stable;
                              mv redis-${VERSION_TAG} redis-stable;
                              tar cvzf redis-stable.tar.gz redis-stable;
                              rm -rf redis-${VERSION_TAG}.tar.gz;
                              shasum -a 256 redis-stable.tar.gz > redis-stable.tar.gz.SHA256SUM;
                              "
else
    echo "Unstable release - skipping stable version update"
fi
