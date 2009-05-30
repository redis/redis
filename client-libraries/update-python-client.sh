#!/bin/sh
rm -rf temp
mkdir temp
cd temp
git clone git://github.com/ludoo/redis.git
cd ..
rm -rf python
mv temp/redis/client-libraries/python python
rm -rf temp
