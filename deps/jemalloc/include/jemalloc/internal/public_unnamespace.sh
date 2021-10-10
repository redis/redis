#!/bin/sh

for nm in `cat $1` ; do
  n=`echo ${nm} |tr ':' ' ' |awk '{print $1}'`
  echo "#undef je_${n}"
done
