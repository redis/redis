#!/bin/sh

# Use smallest possible chunk size.  Immediately purge to minimize
# fragmentation.
export MALLOC_CONF="lg_chunk:0,lg_dirty_mult:-1,decay_time:-1"
