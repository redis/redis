#!/bin/sh

if [ "x${enable_fill}" = "x1" ] ; then
  export MALLOC_CONF="abort:false,zero:false,redzone:true,quarantine:0,junk:alloc"
fi
