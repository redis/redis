# Native Cuckoo tests

Run `make test-cuckoo`; Python 3 standard library and the normal Redis C
toolchain are sufficient. See [native Cuckoo](../../docs/native-cuckoo.md)
for stress, allocator and external-compatibility settings.

The upstream flow and C algorithm cases are retained from RedisBloom v8.11.81.
Native-specific cases cover core lifecycle, ACL, notifications, persistence,
replication and defragmentation. No RLTest, redis-py or module is required.
