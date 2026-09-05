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
* Wildcards are matched from the **certificate**, not from this option: a
  certificate whose SAN is a full-label wildcard (`*.example.com`) matches a
  concrete name configured here (`node.example.com`), while a partial-label
  wildcard in the certificate (`f*.example.com`) never matches. Configuring a
  wildcard as the expected name does not perform wildcard matching.
* A name beginning with a dot (e.g. `.example.com`) is **not** a host name.
  OpenSSL treats it as a *parent domain*: it accepts any subdomain at **any
  depth** (`node.example.com`, `a.b.example.com`, ...) and does **not** accept
  `example.com` itself. A short suffix is correspondingly broad — `.com` accepts
  every name in that TLD, which under a shared or public CA is close to accepting
  any CA-signed certificate. This is a legitimate pattern, but much wider than
  pinning explicit hosts, so Redis logs a warning reporting how many such entries
  are configured, at startup and on `CONFIG SET`, to make the scope visible.
  Prefer explicit host names, a deeper parent (`.dc1.example.com`), or several
  space-separated names.
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

TLS groups
----------

The `tls-groups` option controls the OpenSSL named groups used
during TLS handshakes. The value is passed directly to OpenSSL, so it accepts
the syntax supported by `SSL_CTX_set1_groups_list()` (or the older
`SSL_CTX_set1_curves_list()` API), for example:

    tls-groups X25519:prime256v1

Redis does not filter the list, so any group name accepted by the linked
OpenSSL build can be used.

The setting applies to both the server context and Redis' client context used
for replication, cluster, and other server-to-server TLS connections. The
client and server must have at least one group in common for the handshake to
succeed.

This option requires OpenSSL 1.0.2 or newer, which provides the
`SSL_CTX_set1_curves_list()` API used by Redis. Newer OpenSSL versions also
provide the equivalent `SSL_CTX_set1_groups_list()` API. Building TLS support
against an older OpenSSL fails with a clear compile-time error by default. To
build Redis without this option, define `TLS_NO_GROUPS`, for
example:

    make BUILD_TLS=yes CFLAGS=-DTLS_NO_GROUPS

When the feature is compiled out, setting `tls-groups` causes TLS
context configuration to fail instead of silently ignoring the requested
preference.

`redis-cli` and `redis-benchmark` expose `--tls-groups` when built with
OpenSSL support for named group configuration. When the feature is compiled
out, the command-line option is not accepted, matching the behavior of other
TLS options that depend on OpenSSL build capabilities.

Post-quantum key exchange
-------------------------

`tls-groups` is also how Redis is pointed at a hybrid post-quantum key
exchange. The motivation is "harvest now, decrypt later": an adversary who
records TLS traffic today can store it until a quantum computer is available
and then recover the session keys, so traffic that must stay confidential for
years needs a quantum-resistant key exchange now, well before such a machine
exists.

This affects the key exchange only. Peer authentication continues to rely on
the classical signature algorithms in the configured certificates, so enabling
these groups does not require reissuing certificates.

The relevant groups are hybrids that combine ML-KEM (FIPS 203) with a classical
elliptic curve and derive the session secret from both halves, so the result is
no weaker than the stronger of the two:

| Group                | Classical part | ML-KEM part  |
| -------------------- | -------------- | ------------ |
| `X25519MLKEM768`     | X25519         | ML-KEM-768   |
| `SecP256r1MLKEM768`  | secp256r1      | ML-KEM-768   |
| `SecP384r1MLKEM1024` | secp384r1      | ML-KEM-1024  |

`X25519MLKEM768` is the most widely deployed of the three and is the
recommended choice.

### Requirements

Hybrid ML-KEM groups require OpenSSL 3.5 or newer, which is the release that
added ML-KEM along with these three groups. To see what the OpenSSL that Redis
is linked against actually implements:

    openssl list -tls-groups

Hybrid key exchange is a TLS 1.3 mechanism, so `tls-protocols` must continue to
permit `TLSv1.3`. Restricting it to `TLSv1.2` disables post-quantum key
exchange entirely, regardless of `tls-groups`.

Group names are case-insensitive from OpenSSL 3.5 onwards.

### Enabling it

Note first that OpenSSL 3.5 and newer already offer `X25519MLKEM768` in their
default group list, so two peers that both run OpenSSL 3.5+ negotiate a hybrid
key exchange without any Redis configuration at all. Setting `tls-groups` is
what lets you *require* it, pin the preference order, or restore it if a local
OpenSSL policy has narrowed the defaults.

To prefer post-quantum key exchange while staying interoperable with peers that
do not support it:

    tls-groups "?*X25519MLKEM768 / ?*X25519"

To require it, so that a peer without ML-KEM cannot connect at all:

    tls-groups "X25519MLKEM768"

For a single configuration shared across a fleet whose OpenSSL versions
straddle 3.5, use the plain colon form with a `?` prefix:

    tls-groups "?X25519MLKEM768:X25519:prime256v1"

The `?` prefix means "ignore this group if the implementation is missing", so
nodes on OpenSSL 3.5+ use the hybrid group while older nodes silently fall back
to the classical groups instead of failing to start. Support for `?` was added
in OpenSSL 3.3. The `/` tuple separator and the `*` prefix used in the first
example arrived with OpenSSL 3.5, which is also the first version to provide
ML-KEM, so in practice that syntax is available wherever the hybrid groups are.

### Group ordering matters

In the plain colon form, OpenSSL clients send a predicted key share for the
**first** group in the list only. Every other group is advertised as merely
supported. A server picks the most preferred group it shares with the client,
preferring one the client already sent a key share for so it can avoid a Hello
Retry Request.

The practical consequence is that ordering, not mere support, decides whether a
connection is actually post-quantum. If a classical group is listed first, two
peers that both support `X25519MLKEM768` will still settle on the classical
group, with no error and nothing in the logs to say so. Always list the hybrid
group first.

The `*` prefix removes this sharp edge by requesting a predicted key share for
each group it marks, so `"?*X25519MLKEM768 / ?*X25519"` completes in a single
round trip against both post-quantum and classical peers. Without it, a
hybrid-first list talking to a classical-only peer costs an extra round trip
while the server issues a Hello Retry Request. The trade-off is a slightly
larger `ClientHello`, since two key shares are sent instead of one.

### Performance

ML-KEM's cost is message size rather than CPU time. Measured against OpenSSL
3.6.2 with a single hybrid key share:

| Group            | `ClientHello` | `ServerHello` |
| ---------------- | ------------- | ------------- |
| `X25519`         | 283 bytes     | 122 bytes     |
| `X25519MLKEM768` | 1461 bytes    | 1210 bytes    |

That is roughly 1.2 KB added to the `ClientHello` and 1.1 KB to the
`ServerHello`, about 2.3 KB extra per full handshake, which is the ML-KEM-768
encapsulation key and ciphertext going over the wire. CPU cost is comparable to
the classical group the hybrid is built on. `SecP384r1MLKEM1024` is the
exception: it is substantially more CPU-intensive, mostly because of P-384
itself, and its key exchange messages approach 1700 bytes.

Two consequences worth planning for:

* The cost falls entirely on the handshake, not on steady-state throughput, so
  it matters most for workloads that reconnect often. Persistent connections
  and client-side connection pooling amortize it to near nothing.
* A `ClientHello` of this size no longer fits in a single TCP segment on a
  standard 1500 byte MTU, and some firewalls and middleboxes handle a split
  `ClientHello` badly, which shows up as handshake hangs or timeouts rather
  than a clean error. If this is a concern, listing the hybrid group without a
  `*` prefix keeps the message small, at the cost of an extra round trip when
  the server does prefer the hybrid group.

### Server-to-server connections

As with any `tls-groups` value, the setting applies to Redis' client context as
well as its server context, so it covers replication, the cluster bus, and
`MIGRATE` in addition to ordinary client traffic. Roll it out consistently: a
node configured with a hybrid-only list cannot replicate from, or form a
cluster with, a node whose OpenSSL does not implement that group.

### Failure behavior

If the linked OpenSSL does not implement a requested group and the name is not
`?`-prefixed, the failure is explicit rather than a silent downgrade.
`CONFIG SET tls-groups` is rejected:

    ERR CONFIG SET failed (possibly related to argument 'tls-groups') - Unable
    to update TLS configuration. Check server logs.

and a server started with such a value fails to configure TLS and exits, naming
the list it rejected:

    # Failed to configure TLS groups: X25519MLKEM768
    # Failed to configure TLS. Check logs for more info.

This is the intended behavior for a security policy option: a node that cannot
provide the requested key exchange refuses to serve rather than falling back to
a classical one. Use the `?` prefix when a fallback is what you actually want.

Connections
-----------

All socket operations now go through a connection abstraction layer that hides
I/O and read/write event handling from the caller.

Multi-threading I/O is supported for TLS since Redis 8.0. TLS connections are
assigned to I/O threads like plain TCP connections, and each I/O thread's
event loop drains any connection-level pending data (typical for TLS) via the
connection abstraction layer.

Sync IO for TLS is currently implemented in a hackish way, i.e. making the
socket blocking and configuring socket-level timeout.  This means the timeout
value may not be so accurate, and there would be a lot of syscall overhead.
However I believe that getting rid of syncio completely in favor of pure async
work is probably a better move than trying to fix that. For replication it would
probably not be so hard. For cluster keys migration it might be more difficult,
but there are probably other good reasons to improve that part anyway.

Multi-port
----------

Consider the implications of allowing TLS to be configured on a separate port,
making Redis listening on multiple ports:

1. Startup banner port notification
2. Proctitle
3. How slaves announce themselves
4. Cluster bus port calculation
