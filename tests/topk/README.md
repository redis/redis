# Native Top-K tests

Run `make test-topk` with Python 3 and the ordinary Redis toolchain.
Upstream flow cases and the corrupt-RDB regression are retained; the upstream
print-only unit demo is replaced by assertion-based heap, controlled-count,
increment, distribution and decay workloads. Native cases check lifecycle,
RESP3, ACL, exact-state replication/AOF and real old-module restart upgrades.

See [native Top-K](../../docs/native-topk.md) for compatibility and limitations.
