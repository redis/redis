#!/bin/sh

if [ "x${enable_prof}" = "x1" ] ; then
  export MALLOC_CONF="prof:false"
fi
