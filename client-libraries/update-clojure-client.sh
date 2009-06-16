#!/bin/sh
rm -rf temp
mkdir temp
cd temp
git clone git://github.com/ragnard/redis-clojure.git
cd redis-clojure
rm -rf .git
cd ..
cd ..
rm -rf clojure
mv temp/redis-clojure clojure
rm -rf temp
