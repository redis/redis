#!/bin/bash
echo "Uploading..."
scp /tmp/redis-${1}.tar.gz antirez@antirez.com:/var/virtual/download.redis.io/httpdocs/releases/
echo "Updating web site..."
ssh antirez@antirez.com "cd /var/virtual/download.redis.io/httpdocs; ./update.sh ${1}"
