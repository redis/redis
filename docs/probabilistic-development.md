# Parallel probabilistic datatype development

`feature/native-probabilistic` is the integration branch. Its infrastructure
reserves type/ACL/notification identifiers, provides guarded core lifecycle
hooks, and shares persistence and dependency-free test helpers.

Each datatype PR targets this branch directly. Implementations and private
headers live under `src/probabilistic/`, including `t_<type>.c` command handlers.
Command definitions remain in `src/commands/`; tests remain in
`tests/<type>/`.

Each family adds `src/probabilistic/<type>.mk` to register its objects and
`INCLUDE_<TYPE>` definition. Its `tests/<type>/probabilistic.mk` registers unit
and flow tests. This lets every branch build and test without the other
datatype implementations. The existing `BUILD_BLOOM=no` compatibility switch
disables the five migrated families; HyperLogLog remains an always-built core
datatype.

Use `make -C src redis-server redis-cli` to build core, and
`make test-probabilistic` to test all families present in the checkout.
`make test`, event CI and Ubuntu nightly also invoke this aggregate target.
Shared Python and C test helpers live in `tests/probabilistic/`.

Review and merge the Bloom, CMS, Cuckoo, Top-K and t-digest PRs independently.
Move HyperLogLog in a separate, behavior-preserving PR. Merge RedisBloom
retirement and cross-family validation last, after all five replacements land.
Partial migration branches must not auto-load the conflicting RedisBloom module;
they are development builds, not complete production replacements.

The final integration PR targets `unstable`. Regenerate `src/commands.def`
with `python3 utils/generate-command-code.py` after resolving command-table
merge conflicts. Do not manually splice generated command metadata.

Upgrade compatibility is required: preserve existing keys, contents and TTLs.
Backward readability by the old module is not a requirement.
