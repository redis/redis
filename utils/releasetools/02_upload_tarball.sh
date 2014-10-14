#!/bin/bash
echo "Uploading..."
scp /tmp/redis-${1}.tar.gz antirez@antirez.com:/var/virtual/download.redis.io/httpdocs/releases/
echo "Updating web site... (press any key if it is a stable release, or Ctrl+C)"
read x
ssh antirez@antirez.com "cd /var/virtual/download.redis.io/httpdocs; ./update.sh ${1}"
