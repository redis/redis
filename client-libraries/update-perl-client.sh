#!/bin/sh
rm -rf temp
mkdir temp
cd temp
svn checkout svn://svn.rot13.org/Redis/ 
find . -name '.svn' -exec rm -rf {} \; 2> /dev/null
cd ..
rm -rf perl
mv temp/Redis perl
rm -rf temp
