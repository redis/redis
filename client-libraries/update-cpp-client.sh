#!/bin/sh
rm -rf temp
mkdir temp
cd temp
git clone git://github.com/fictorial/redis-cpp-client.git
cd redis-cpp-client
rm -rf .git
cd ..
cd ..
rm -rf cpp
mv temp/redis-cpp-client cpp
rm -rf temp
