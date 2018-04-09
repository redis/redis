#!/bin/bash
rm -rf s2n
rm -rf openssl

mkdir openssl
cd openssl
curl -LO https://www.openssl.org/source/openssl-1.0.2-latest.tar.gz
tar -xzvf openssl-1.0.2-latest.tar.gz
cp -r openssl-1.0.2*/* .
rm -f openssl-1.0.2-latest.tar.gz
echo "Downloaded latest version of openssl 1.0.2"

cd ..
git clone https://github.com/awslabs/s2n.git s2n
echo "Downloaded latest version of s2n"

