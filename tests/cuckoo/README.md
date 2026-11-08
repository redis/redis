# Native Cuckoo tests

Build Redis, then run from the repository root:

```sh
python3 -m pip install -r tests/bloom/requirements.txt
./runtest-cuckoo
```

The runner reuses Bloom's RLTest setup (`tests/bloom/common.py`) and dependencies.
It starts isolated native Redis servers without loading RedisBloom.

`test_commands.py` is copied unchanged from RedisBloom's `tests/flow/test_cuckoo.py`.
`test_resp3.py` contains the Cuckoo section of its shared `test_resp3.py`.
The existing insufficient-memory test remains skipped upstream because its
capacity is outside the accepted range. Some tests allocate large filters.

The native integration tests and C tests run separately:

```sh
./runtest --single unit/cuckoo
# Requires a build with REDIS_CFLAGS=-DREDIS_TEST:
src/redis-server test cuckoo --accurate
src/redis-server test cuckoo-object --accurate
```

`test_acl.py` and `test_configs.py` cover the Cuckoo cases from the module's
shared suites. Configuration at module load is tested through native server
configuration instead; range errors use Redis core's wording. ACL checks use
valid arity because core checks arity before command permissions.

`test_replication.py` keeps the module's Cuckoo write scenarios and adds
`CF.COMPACT`, which is a write command in core. Its synchronization and data
comparison helpers are shared with Bloom in `tests/bloom/replication_utils.py`.
