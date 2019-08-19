TLS Support -- Work In Progress
===============================

This is a brief note to capture current thoughts/ideas and track pending action
items.

Getting Started
---------------

### Building

To build with TLS support you'll need OpenSSL development libraries (e.g.
libssl-dev on Debian/Ubuntu).

Run `make BUILD_TLS=yes`.

### Tests

To run Redis test suite with TLS, you'll need TLS support for TCL (i.e.
`tcl-tls` package on Debian/Ubuntu).

1. Run `./utils/gen-test-certs.sh` to generate a root CA and a server
   certificate.

2. Run `./runtest --tls` or `./runtest-cluster --tls` to run Redis and Redis
   Cluster tests in TLS mode.

### Running manually

To manually run a Redis server with TLS mode (assuming `gen-test-certs.sh` was
invoked so sample certificates/keys are available):

    ./src/redis-server --tls-port 6379 --port 0 \
        --tls-cert-file ./tests/tls/redis.crt \
        --tls-key-file ./tests/tls/redis.key \
        --tls-ca-cert-file ./tests/tls/ca.crt

To connect to this Redis server with `redis-cli`:

    ./src/redis-cli --tls \
        --cert ./tests/tls/redis.crt \
        --key ./tests/tls/redis.key \
        --cacert ./tests/tls/ca.crt

This will disable TCP and enable TLS on port 6379. It's also possible to have
both TCP and TLS available, but you'll need to assign different ports.

To make a Replica connect to the master using TLS, use `--tls-replication yes`,
and to make Redis Cluster use TLS across nodes use `--tls-cluster yes`.

**NOTE: This is still very much work in progress and some configuration is still
missing or may change.**

Connections
-----------

Connection abstraction API is mostly done and seems to hold well for hiding
implementation details between TLS and TCP.

1. Multi-threading I/O is not supported.  The main issue to address is the need
   to manipulate AE based on OpenSSL return codes.  We can either propagate this
   out of the thread, or explore ways of further optimizing MT I/O by having
   event loops that live inside the thread and borrow connections in/out.

2. Finish cleaning up the implementation.  Make sure all error cases are handled
   and reflected into connection state, connection state validated before
   certain operations, etc.
    - Clean (non-errno) interface to report would-block.
    - Consistent error reporting.

3. Sync IO for TLS is currently implemented in a hackish way, i.e. making the
   socket blocking and configuring socket-level timeout.  This means the timeout
   value may not be so accurate, and there would be a lot of syscall overhead.
   However I believe that getting rid of syncio completely in favor of pure
   async work is probably a better move than trying to fix that. For replication
   it would probably not be so hard. For cluster keys migration it might be more
   difficult, but there are probably other good reasons to improve that part
   anyway.

TLS Features
------------

1. Add metrics to INFO.
2. Add certificate authentication configuration (i.e. option to skip client
auth, master auth, etc.).
3. Add TLS cipher configuration options.
4. [Optional] Add session caching support. Check if/how it's handled by clients
   to assess how useful/important it is.


redis-benchmark
---------------

The current implementation is a mix of using hiredis for parsing and basic
networking (establishing connections), but directly manipulating sockets for
most actions.

This will need to be cleaned up for proper TLS support. The best approach is
probably to migrate to hiredis async mode.

redis-cli
---------
1. Support tls in --slave and --rdb


Others
------

Consider the implications of allowing TLS to be configured on a separate port,
making Redis listening on multiple ports.

This impacts many things, like
1. Startup banner port notification
2. Proctitle
3. How slaves announce themselves
4. Cluster bus port calculation
