#!/bin/sh
rm -rf temp
mkdir temp
cd temp
git clone git://github.com/nrk/redis-lua.git
cd redis-lua
rm -rf .git
cd ..
cd ..
rm -rf lua
mv temp/redis-lua lua
rm -rf temp
