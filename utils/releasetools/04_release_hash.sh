#!/bin/bash
SHA=$(curl -s http://download.redis.io/releases/redis-${1}.tar.gz | shasum -a 256 | cut -f 1 -d' ')
ENTRY="hash redis-${1}.tar.gz sha256 $SHA http://download.redis.io/releases/redis-${1}.tar.gz"
echo $ENTRY >> ~/hack/redis-hashes/README
vi ~/hack/redis-hashes/README
echo "Press any key to commit, Ctrl-C to abort)."
read yes
(cd ~/hack/redis-hashes; git commit -a -m "${1} hash."; git push)
