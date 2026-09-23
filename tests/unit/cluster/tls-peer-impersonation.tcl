# SPEC-01 Part B — cluster-bus node-ID impersonation is blocked by accept-side
# tls-expected-peer-name verification.
#
# Background (Threat model B): the cluster bus has no per-message authentication;
# the sender of a packet is identified solely by the (public) node ID in the
# header. A machine holding a certificate signed by the trusted CA but WITHOUT
# the cluster identity SAN could therefore open an *inbound* cluster-bus
# connection and forge a FAIL packet spoofing a real member's node ID, with no
# handshake and no return link. Outbound verification (Part A) cannot stop this,
# because the victim never dials the attacker.
#
# Part B verifies the connecting peer's client certificate SAN on accept
# (connSetVerifyName in clusterAcceptHandler), so a certificate lacking the
# cluster identity cannot complete the TLS handshake and its forged packets are
# never processed.
#
# The cluster below runs with tls-expected-peer-name active: every node uses a
# cert carrying the cluster SAN (redis.local) and verifies it in BOTH directions.
# The "attacker" connects with tests/tls/redis.crt, which chains to the same CA
# but has no SAN.

if {$::tls} {
    set san_crt [format "%s/tests/tls/san.crt" [pwd]]
    set san_key [format "%s/tests/tls/san.key" [pwd]]

    # Use the SAN cert for BOTH the server and client roles: with Part B, a node's
    # outbound (client) cert is verified by the peer on accept, so it too must
    # carry the cluster identity SAN. (The test harness otherwise configures a
    # separate SAN-less client cert.)
    start_cluster 3 0 [list tags {external:skip cluster tls} \
            overrides [list tls-cert-file $san_crt tls-key-file $san_key \
                            tls-client-cert-file $san_crt tls-client-key-file $san_key \
                            tls-expected-peer-name redis.local]] {

        test "Cluster forms with tls-expected-peer-name active (both directions)" {
            # Exercises the SAN check on both the outbound (Part A) and the
            # accept-side (Part B) cluster-bus connections: legitimate members
            # present a cert carrying the expected SAN, so the cluster still forms.
            wait_for_cluster_state ok
        }

        test "Cluster bus rejects a forged FAIL from a non-member cert (Part B)" {
            set CLUSTERMSG_TYPE_FAIL 3
            set CLUSTERMSG_MIN_LEN 2256
            set CLUSTER_NAMELEN 40

            # Victim = node 0.
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

            # The attacker cert (redis.crt) chains to the CA but carries no cluster
            # SAN. With Part B the victim verifies the client cert's SAN on accept
            # and rejects the handshake, so the write never reaches packet
            # processing. The client side may raise on the failed handshake, hence
            # the catch.
            catch {
                set fd [::tls::socket \
                    -cafile "$::tlsdir/ca.crt" \
                    -certfile "$::tlsdir/redis.crt" \
                    -keyfile "$::tlsdir/redis.key" \
                    $target_host $target_bus_port]
                fconfigure $fd -translation binary -buffering full
                puts -nonewline $fd $packet
                flush $fd
            }
            catch {close $fd}

            # The victim rejects the attacker's client certificate on accept (no
            # matching SAN), so the handshake fails and the forged FAIL is never
            # processed. Wait for the accept-side rejection to be logged rather than
            # sleeping a fixed window; this is the positive signal that the connection
            # was refused before any packet could be handled.
            wait_for_log_messages 0 {"*Error accepting cluster node connection*certificate verify failed*"} 0 50 100

            # The forged FAIL must not have been processed.
            assert_equal 0 [count_log_message 0 "FAIL message received"]

            # Node 2 must not be flagged failed in node 0's view.
            set node2 {}
            foreach n [get_cluster_nodes 0] {
                if {[dict get $n id] eq $failed_target} { set node2 $n; break }
            }
            assert {$node2 ne {}}
            assert_equal 0 [cluster_has_flag $node2 fail]

            # The victim remains responsive.
            assert_equal [R 0 PING] {PONG}
        }
    }
}
