TLS Support
===========

Getting Started
---------------

### Building

To build with TLS support you'll need OpenSSL development libraries (e.g.
libssl-dev on Debian/Ubuntu).

To build TLS support as Redis built-in:
Run `make BUILD_TLS=yes`.

Or to build TLS as Redis module:
Run `make BUILD_TLS=module`.

Note that sentinel mode does not support TLS module.

### Tests

To run Redis test suite with TLS, you'll need TLS support for TCL (i.e.
`tcl-tls` package on Debian/Ubuntu).

1. Run `./utils/gen-test-certs.sh` to generate a root CA and a server
   certificate.

2. Run `./runtest --tls` or `./runtest-cluster --tls` to run Redis and Redis
   Cluster tests in TLS mode.

3. Run `./runtest --tls-module` or `./runtest-cluster --tls-module` to
   run Redis and Redis cluster tests in TLS mode with Redis module.

### Running manually

To manually run a Redis server with TLS mode (assuming `gen-test-certs.sh` was
invoked so sample certificates/keys are available):

For TLS built-in mode:
    ./src/redis-server --tls-port 6379 --port 0 \
        --tls-cert-file ./tests/tls/redis.crt \
        --tls-key-file ./tests/tls/redis.key \
        --tls-ca-cert-file ./tests/tls/ca.crt

For TLS module mode:
    ./src/redis-server --tls-port 6379 --port 0 \
        --tls-cert-file ./tests/tls/redis.crt \
        --tls-key-file ./tests/tls/redis.key \
        --tls-ca-cert-file ./tests/tls/ca.crt \
        --loadmodule src/redis-tls.so

To connect to this Redis server with `redis-cli`:

    ./src/redis-cli --tls \
        --cert ./tests/tls/redis.crt \
        --key ./tests/tls/redis.key \
        --cacert ./tests/tls/ca.crt

This will disable TCP and enable TLS on port 6379. It's also possible to have
both TCP and TLS available, but you'll need to assign different ports.

Note why that `redis-cli` invocation passes a certificate of its own: by default
clients on a TLS port must present one that verifies against the configured CA,
replicas connecting to a master included. `tls-auth-clients no` accepts no client
certificate at all, and `tls-auth-clients optional` validates one only when it is
offered. Both relax the client port alone — see "Cluster bus protection" below for
why neither can relax the cluster bus. Setting `tls-auth-clients-user CN`
additionally maps a validated certificate's Common Name to a Redis ACL user, so
the certificate authenticates the client without an `AUTH` command.

To make a Replica connect to the master using TLS, use `--tls-replication yes`,
and to make Redis Cluster use TLS across nodes use `--tls-cluster yes`. Note that
a cluster node started without `tls-cluster` warns that its bus is
unauthenticated; see "Cluster bus protection" below.

Cluster bus protection
----------------------

The cluster bus protocol has no authentication of its own: any host able to reach
a node's cluster bus port can speak the protocol — a `MEET` from an unknown host
is enough to have it added as a node, with its gossip trusted — and so threaten
the whole cluster. What authenticates the bus is `tls-cluster`, which makes every
peer present a certificate that verifies against the configured CA — in both the
accepting and the connecting direction, and `tls-cluster` also makes configuring
a CA mandatory. This does not depend on `tls-auth-clients`, which governs the
client port only.

For that reason a node started with `cluster-enabled yes` but no `tls-cluster`
records in its log that its cluster bus is unauthenticated. Setting
`cluster-bus-port-protected-mode` to **yes** — it defaults to **no** — turns that
into a refusal to start, so that keeping an unauthenticated cluster bus becomes an
explicit choice, acknowledging that the port is reachable by trusted hosts only —
because a firewall, a VPN or a private network keeps untrusted hosts away from it.
The option has no effect outside cluster mode, as only a cluster node opens a
cluster bus port.

Connections
-----------

All socket operations now go through a connection abstraction layer that hides
I/O and read/write event handling from the caller.

**Multi-threading I/O is not currently supported for TLS**, as a TLS connection
needs to do its own manipulation of AE events which is not thread safe. The
solution is probably to manage independent AE loops for I/O threads and longer
term association of connections with threads. This may potentially improve
overall performance as well.

Sync IO for TLS is currently implemented in a hackish way, i.e. making the
socket blocking and configuring socket-level timeout.  This means the timeout
value may not be so accurate, and there would be a lot of syscall overhead.
However I believe that getting rid of syncio completely in favor of pure async
work is probably a better move than trying to fix that. For replication it would
probably not be so hard. For cluster keys migration it might be more difficult,
but there are probably other good reasons to improve that part anyway.

To-Do List
----------

- [ ] redis-benchmark support. The current implementation is a mix of using
  hiredis for parsing and basic networking (establishing connections), but
  directly manipulating sockets for most actions. This will need to be cleaned
  up for proper TLS support. The best approach is probably to migrate to hiredis
  async mode.
- [ ] redis-cli `--slave` and `--rdb` support.

Multi-port
----------

Consider the implications of allowing TLS to be configured on a separate port,
making Redis listening on multiple ports:

1. Startup banner port notification
2. Proctitle
3. How slaves announce themselves
4. Cluster bus port calculation
