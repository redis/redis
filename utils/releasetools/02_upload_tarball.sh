#!/bin/bash
scp /tmp/redis-${1} antirez@antirez.com:/var/virtual/download.redis.io/httpdocs/releases/
ssh antirez@antirez.com 'cd /var/virtual/download.redis.io/httpdocs; ./update.sh ${1}'
