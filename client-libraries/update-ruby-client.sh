#!/bin/sh
rm -rf temp
mkdir temp
cd temp
git clone git://github.com/ezmobius/redis-rb.git
cd redis-rb
rm -rf .git
cd ..
cd ..
rm -rf ruby
mv temp/redis-rb ruby
rm -rf temp
