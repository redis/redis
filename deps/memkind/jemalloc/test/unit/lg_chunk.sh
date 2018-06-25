#!/bin/sh

# Make sure that opt.lg_chunk clamping is sufficient.  In practice, this test
# program will fail a debug assertion during initialization and abort (rather
# than the test soft-failing) if clamping is insufficient.
export MALLOC_CONF="lg_chunk:0"
