#!/bin/sh

if [ "x${enable_prof}" = "x1" ] ; then
  export MALLOC_CONF="prof:true,prof_accum:true,prof_active:false,lg_prof_sample:0,lg_prof_interval:0"
fi


