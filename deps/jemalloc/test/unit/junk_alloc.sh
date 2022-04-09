#!/bin/sh

if [ "x${enable_fill}" = "x1" ] ; then
  export MALLOC_CONF="abort:false,zero:false,junk:alloc"
fi
