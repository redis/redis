#!/bin/bash
rm -rf s2n

git clone https://github.com/awslabs/s2n.git s2n
cd s2n
git checkout 2f06e444be294a90b0ae55b1f4b2b46af0bdf412
cd ..
echo "Downloaded stable version of s2n"

