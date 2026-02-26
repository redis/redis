#!/bin/sh

# Immediately purge to minimize fragmentation.
export MALLOC_CONF="dirty_decay_ms:0,muzzy_decay_ms:0"
