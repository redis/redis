# SPEC-01 Part A / §A.6.2 — cluster-bus node-ID impersonation regression test.
#
# This test proves that outbound peer-name verification (Part A,
# tls-expected-peer-name enforced in connTLSConnect) is NOT sufficient on its own:
# a machine holding a certificate signed by the trusted CA but WITHOUT the cluster
# identity SAN can still open an *inbound* cluster-bus connection and impersonate a
# real cluster member by spoofing its (public) node ID in the packet header. No
# handshake and no return link are required, so the outbound SAN check never fires.
#
# The cluster below runs with Part A fully active: every node uses a cert carrying
# the cluster SAN (redis.local) and verifies tls-expected-peer-name on its outbound
# connections. The "attacker" connects with tests/tls/redis.crt, which chains to the
# same CA but has no SAN, and sends a forged FAIL packet. The victim still processes
# it, because the accept side only validates the CA chain today.
#
# >>> TRIPWIRE FOR PART B <<<
# This test currently asserts the *vulnerable* behavior (the forged FAIL is
# processed) so it passes and documents the gap. When Part B (accept-side
# tls-expected-peer-name verification in connCreateAcceptedTLS / clusterAcceptHandler)
# is implemented, the victim must REJECT the attacker's TLS connection and never
# process the FAIL. At that point this test will start failing and MUST be inverted:
#   - assert the "FAIL message received" log line does NOT appear, and
#   - optionally assert the attacker's connection is dropped at the TLS handshake.

if {$::tls} {
    set san_crt [format "%s/tests/tls/san.crt" [pwd]]
    set san_key [format "%s/tests/tls/san.key" [pwd]]

    start_cluster 3 0 [list tags {external:skip cluster tls} \
            overrides [list tls-cert-file $san_crt tls-key-file $san_key \
                            tls-expected-peer-name redis.local]] {

        test "Cluster forms with tls-expected-peer-name active (Part A)" {
            # Sanity: Part A verification does not break legitimate cluster bus
            # connections, since every node's cert carries the expected SAN.
            wait_for_cluster_state ok
        }

        test "Cluster bus accepts a forged FAIL from a non-member cert (Part A insufficient)" {
            set CLUSTERMSG_TYPE_FAIL 3
            set CLUSTERMSG_MIN_LEN 2256
            set CLUSTER_NAMELEN 40

            # Victim = node 0. It will process the forged packet.
            set target_host [srv 0 host]
            set target_bus_port [expr {[srv 0 port] + 10000}]

            # Spoof node 1 (a real, trusted master) as the sender, and falsely
            # report node 2 as failing. Both IDs are public (CLUSTER MYID).
            set spoofed_sender [R 1 CLUSTER MYID]
            set failed_target  [R 2 CLUSTER MYID]

            set sender_port [srv -1 port]
            set sender_cport [expr {$sender_port + 10000}]

            # FAIL packet: header (2256) + clusterMsgDataFail.about.nodename (40).
            set totlen [expr {$CLUSTERMSG_MIN_LEN + $CLUSTER_NAMELEN}]
            set packet [build_cluster_bus_header $spoofed_sender $sender_port $sender_cport \
                $CLUSTERMSG_TYPE_FAIL $totlen]
            append packet [binary format a${CLUSTER_NAMELEN} $failed_target]

            set loglines [count_log_lines 0]

            # The attacker cert (redis.crt) chains to the CA but carries no cluster
            # SAN. Under Part A the victim's accept side validates only the chain, so
            # this connection is accepted.
            set fd [::tls::socket \
                -cafile "$::tlsdir/ca.crt" \
                -certfile "$::tlsdir/redis.crt" \
                -keyfile "$::tlsdir/redis.key" \
                $target_host $target_bus_port]
            fconfigure $fd -translation binary -buffering full
            puts -nonewline $fd $packet
            flush $fd

            # VULNERABLE BEHAVIOR (Part A): the victim processes the forged FAIL and
            # logs it. See the TRIPWIRE note at the top of this file: invert this
            # assertion once Part B rejects the connection.
            wait_for_log_messages 0 {"*FAIL message received*about*"} $loglines 50 100
            close $fd

            # The victim remains responsive after handling the forged packet.
            assert_equal [R 0 PING] {PONG}
        }
    }
}
