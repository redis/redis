#!/bin/sh
rm -rf temp
mkdir temp
cd temp
git clone git://github.com/acrosa/scala-redis.git
cd scala-redis
rm -rf .git
cd ..
cd ..
rm -rf scala
mv temp/scala-redis scala
rm -rf temp
