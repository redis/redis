#!/bin/sh

XMALLOC_STR=""
if [ "x${enable_xmalloc}" = "x1" ] ; then
  XMALLOC_STR="xmalloc:false,"
fi

export MALLOC_CONF="${XMALLOC_STR}experimental_infallible_new:false"
