#!/bin/sh

# Some screwy-looking slab sizes.
export MALLOC_CONF="slab_sizes:1-4096:17|100-200:1|128-128:2"
