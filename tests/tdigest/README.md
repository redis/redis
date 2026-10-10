# Native t-digest tests

Run `make test-tdigest`. Python tests use only the standard library.
Upstream flow cases retain their assertions with RLTest/redis-py wrappers
removed and numpy.nan replaced by math.nan. The save/reload test now requires
preservation of buffered nodes under native encoding version 1.

All 28 upstream algorithm tests, capacity boundaries and adversarial sort
complexity tests are included. The upstream mixed-type defrag workload also
runs here; it requires jemalloc and verifies surviving values for all families.
Native tests cover exact-state persistence, read/write replication interleaving,
RESP3, ACL and legacy restart upgrades.
