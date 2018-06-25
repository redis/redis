#!/bin/sh

# Keep in sync with definition in quarantine.c.
export QUARANTINE_SIZE=8192

if [ "x${enable_fill}" = "x1" ] ; then
  export MALLOC_CONF="abort:false,junk:true,redzone:true,quarantine:${QUARANTINE_SIZE}"
fi
