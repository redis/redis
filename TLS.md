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

To make a Replica connect to the master using TLS, use `--tls-replication yes`,
and to make Redis Cluster use TLS across nodes use `--tls-cluster yes`.

Peer certificate verification
-----------------------------

By default, Redis TLS validates only that a peer certificate **chains to the
trusted CA** (`tls-ca-cert-file` / `tls-ca-cert-dir`) and is not expired. It does
**not** verify that the certificate belongs to the specific peer being contacted.

### Trust model and the CA assumption

This default is safe **only under a dedicated, per-cluster CA**: since that CA
signs certificates exclusively for your own nodes, "chains to the CA" is
equivalent to "is a legitimate peer". This is the recommended Redis TLS
deployment model.

If you instead use a **shared, organizational, or public CA**, that CA also signs
certificates for machines that are not part of your deployment. In that case CA
validation alone is insufficient:

* A machine-in-the-middle holding any certificate signed by the same CA can
  impersonate a master/peer on outbound connections (replication, cluster bus,
  `MIGRATE`) and, for replication, capture the `AUTH <masteruser> <masterauth>`
  credentials a replica sends.
* Because the cluster bus has no per-message authentication (a message's sender
  is identified only by the public node ID in its header), such a certificate can
  also open an inbound cluster-bus connection and forge messages that impersonate
  a real member — for example a `FAIL` message that marks a healthy node as
  failed.

### `tls-expected-peer-name`

To close this gap under a shared CA, set `tls-expected-peer-name`. When set, the
peer certificate must additionally carry one of the configured names in its
Subject Alternative Name (SAN) field (CN is used only as a fallback), verified as
part of CA chain validation. Issue every node's certificate with a shared
cluster-identity SAN (e.g. `node.my-redis-cluster.example.com`) and set this
option to that name on every node; a certificate signed by the same CA but
lacking the name is then rejected.

The expected name is taken from local configuration only — never from the address
dialed or from any data received over the wire (such as a gossip-announced
hostname), since the whole purpose is to distinguish a real peer from an
impostor.

The check is enforced in **both directions** of server-to-server TLS:

* **Outbound** — the peer's *server* certificate is verified when this node dials
  a master, cluster peer, or `MIGRATE` target.
* **Inbound cluster bus** — the connecting peer's *client* certificate is verified
  on accept, which is what blocks the impersonation/forgery described above.

Because a node's outbound (client) certificate is now verified by its peers, the
identity SAN must be present on **whichever certificate the node presents in both
roles**. In practice this means the SAN must be on `tls-client-cert-file` (used
for outbound cluster/replication connections) as well as on `tls-cert-file`; when
no separate client certificate is configured, `tls-cert-file` is used for both
roles and only needs the SAN once. If a separate client certificate without the
SAN is configured, peers will reject this node's inbound cluster-bus connections
and the cluster will not form.

Notes:

* Multiple names may be supplied as a single space-separated value (quote it in
  the config file, e.g. `tls-expected-peer-name "a.example.com b.example.com"`);
  a match against any one of them succeeds.
* Full-label wildcards (`*.example.com`) match; partial-label wildcards
  (`f*.example.com`) do not.
* The option is opt-in and defaults to unset (no peer-name check), preserving the
  previous behavior.
* It does **not** apply to ordinary client connections on the data port, whose
  identity and authorization are handled by `AUTH`/ACL (and optionally
  `tls-auth-clients-user`).
* Peer-name verification uses the OpenSSL `X509_VERIFY_PARAM` host API, available
  since OpenSSL 1.0.2. Building the TLS support against an older OpenSSL fails
  with an error by default. Define `TLS_NO_PEER_NAME_VERIFICATION`
  (e.g. `make BUILD_TLS=yes CFLAGS=-DTLS_NO_PEER_NAME_VERIFICATION`) to compile
  the feature out — either to build against an older OpenSSL, or to disable it on
  any OpenSSL version. When compiled out, if `tls-expected-peer-name` is set each
  affected connection logs a warning and proceeds without the name check (CA
  chain validation still applies).

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
