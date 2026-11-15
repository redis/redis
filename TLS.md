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

2. Run `./runtest --tls` to run Redis and Redis Cluster tests in TLS mode.

3. Run `./runtest --tls-module` to run Redis and Redis Cluster tests in TLS
   mode with Redis module.

### Running manually

To manually run a Redis server with TLS mode (assuming `gen-test-certs.sh` was
invoked so sample certificates/keys are available):

For TLS built-in mode:
    ./src/redis-server --tls-port 6379 --port 0 \
        --tls-cert-file ./tests/tls/server.crt \
        --tls-key-file ./tests/tls/server.key \
        --tls-ca-cert-file ./tests/tls/ca.crt

For TLS module mode:
    ./src/redis-server --tls-port 6379 --port 0 \
        --tls-cert-file ./tests/tls/server.crt \
        --tls-key-file ./tests/tls/server.key \
        --tls-ca-cert-file ./tests/tls/ca.crt \
        --loadmodule src/redis-tls.so

To connect to this Redis server with `redis-cli`:

    ./src/redis-cli --tls \
        --cert ./tests/tls/redis.crt \
        --key ./tests/tls/redis.key \
        --cacert ./tests/tls/ca.crt

This will disable TCP and enable TLS on port 6379. It's also possible to have
both TCP and TLS available, but you'll need to assign different ports.

The server presents `server.crt` because `redis-cli` verifies, by default, that
the server certificate carries the address it connects to: `server.crt` carries
`127.0.0.1`, `::1` and `localhost` in its Subject Alternative Name, while
`redis.crt` carries no identity at all and would be rejected with an
explanation. See "Client tools" under "Peer certificate verification" below.

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
a cluster node refuses to start without `tls-cluster` unless the requirement is
explicitly waived; see "Cluster bus protection" below.

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

For that reason `cluster-bus-port-protected-mode` defaults to **yes**, and a node
started with `cluster-enabled yes` refuses to start unless `tls-cluster` is
enabled. Running an unauthenticated cluster bus remains supported, but it has to
be stated explicitly with `cluster-bus-port-protected-mode no`, acknowledging
that the port is reachable by trusted hosts only — because a firewall, a VPN or a
private network keeps untrusted hosts away from it. A node started that way
records the fact in its log. The option has no effect outside cluster mode, as
only a cluster node opens a cluster bus port.

Peer certificate verification
-----------------------------

Two different rules apply, depending on who is connecting:

* **Client tools** — `redis-cli`, `redis-benchmark` and Sentinel's connections to
  the instances it monitors — verify the server certificate's **identity** by
  default, the way a browser or `curl` does: besides chaining to the trusted CA,
  the certificate must carry the name or address the tool connected to. See
  "Client tools" below.
* **Server-to-server** connections — replication, the cluster bus and `MIGRATE` —
  validate by default only that the peer certificate **chains to the trusted CA**
  (`tls-ca-cert-file` / `tls-ca-cert-dir`) and is not expired, and verify an
  identity only when `tls-expected-peer-name` is configured. The rest of this
  section explains that model.

### Client tools: redis-cli, redis-benchmark and Sentinel

By default these tools require the server certificate to carry the **expected
identity** in its Subject Alternative Name (SAN) — the equivalent of libpq's
`sslmode=verify-full`. Without this, any certificate issued by a trusted CA is
accepted whatever identity it carries, so under a shared or public CA the holder
of an unrelated certificate could impersonate the server and capture the `AUTH`
credentials these tools send.

The identity is taken from the most explicit source available:

1. `--tls-expected-peer-name <names>` (redis-cli, redis-benchmark) or the
   `tls-expected-peer-name` config (Sentinel): an explicit identity, independent
   of the address dialed — a space-separated list of DNS names, any of which
   may match. The tools' flag additionally accepts a single IP address; the
   server config takes DNS names only (see its section below). This fits
   deployments where every node's certificate carries a shared cluster-identity
   SAN, including cluster mode where node addresses are learned from the
   cluster itself.
2. `--sni <name>`: the declared logical name of the server. When you connect to
   an address but the server is known by a name, `--sni` both announces that
   name to the server and is the identity the certificate is verified against.
3. The host that was dialed (`-h` for the tools; the configured or announced
   address of an instance for Sentinel): an IP address is matched against the
   certificate's `IP` SAN entries, a DNS name against its `DNS` entries (with
   the Common Name as a fallback when the certificate has no DNS entries).

Sentinel has no `--sni` equivalent, so for it the choice is simply: the configured
`tls-expected-peer-name` if there is one, otherwise the instance's address.

Separately from which identity is verified, the dialed host — for Sentinel, the
instance's address — is also sent to the server as SNI whenever it is a DNS name.
Redis itself serves a single certificate and ignores SNI, so this matters for a
TLS proxy or load balancer in front of Redis, which may route the connection by
SNI or present the certificate issued for that name.

The three verification levels, and how to select them:

| Level | redis-cli / redis-benchmark | Sentinel | Verifies |
|---|---|---|---|
| full (default) | — | `sentinel tls-verify-hostnames yes` | CA chain **and** identity |
| chain only | `--no-verify-hostname` | `sentinel tls-verify-hostnames no` | CA chain |
| none | `--insecure` | — | nothing |

`--no-verify-hostname` cannot be combined with `--tls-expected-peer-name`, nor can
`--insecure`: the former contradicts an explicit identity, the latter verifies
nothing at all, so an identity could never be checked.

**Certificate requirements.** A certificate must carry the identity the tools
verify. In particular an address that is dialed as an **IP** — the common case,
and Sentinel's default since `resolve-hostnames` defaults to `no` — must appear
as an `IP:` SAN entry; there is no fallback to the Common Name for IP addresses.
Certificates issued for DNS names only, or with no SAN at all, fail with an
explanation naming the expected identity and the option to relax the check. When
upgrading, review the certificates of every instance the tools and Sentinel
connect to; `utils/gen-test-certs.sh` shows how to issue certificates carrying
IP and DNS identities. Alternatively, issue every instance's certificate with a
shared name and pin it with `tls-expected-peer-name`, which replaces the address
check.

Sentinel logs an identity mismatch as a `-tls-identity-mismatch` event at WARNING
level (rate limited per link), naming the expected identity and the remedies,
since a Sentinel whose instance certificates lack their addresses would otherwise
lose every link with nothing but DEBUG-level reconnection events to show for it.
`sentinel tls-verify-hostnames` can be changed at runtime with
`SENTINEL CONFIG SET`; since the identity is checked when a link is established,
the change applies to connections made from then on, not to links already up.
It governs only the address check: a configured `tls-expected-peer-name` is
verified whether it is `yes` or `no`.

A name beginning with a dot given to `--tls-expected-peer-name` is a parent-domain
pattern (see the note under `tls-expected-peer-name` below); the tools accept it
and print a warning about its wider scope.

Unix-socket connections have no host name or address to verify against, so the
identity check applies to them only when `--sni` or `--tls-expected-peer-name`
provides one.

Identity verification uses the OpenSSL `X509_VERIFY_PARAM` host API, available
since OpenSSL 1.0.2, under the same `TLS_NO_PEER_NAME_VERIFICATION` build flag
described below. When compiled out, the tools verify the CA chain only and say so
once; an explicit `--tls-expected-peer-name` is refused rather than silently
ignored.

### Server-to-server connections: trust model and the CA assumption

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
* Only DNS names are accepted; an IP address is rejected at configuration time.
  This option names peers by stable DNS identities, and OpenSSL holds a single
  expected IP address that must match in addition to any host names, which does
  not fit a list where any entry may match. To verify a peer by its address, use
  Sentinel's `tls-verify-hostnames` or the identity check the tools perform by
  default.
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
