#!/bin/sh

if [ "x${enable_prof}" = "x1" ] ; then
  export MALLOC_CONF="prof:true,prof_thread_active_init:false,lg_prof_sample:0"
fi
