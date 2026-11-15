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
    # Node $idx's view of the member with node ID $id, or {} if unknown.
    proc cluster_node_view {idx id} {
        foreach n [get_cluster_nodes $idx] {
            if {[dict get $n id] eq $id} { return $n }
        }
        return {}
    }

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

            # The forged FAIL must not have been processed. The next test is the
            # positive control for this line: the same packet, sent through a
            # certificate carrying the SAN, does produce it.
            assert_equal 0 [count_log_message 0 "FAIL message received"]

            # Node 2 must not be flagged failed in node 0's view.
            set node2 [cluster_node_view 0 $failed_target]
            assert {$node2 ne {}}
            assert_equal 0 [cluster_has_flag $node2 fail]

            # The victim remains responsive.
            assert_equal [R 0 PING] {PONG}
        }

        # --- TLS session resumption and the cluster-bus identity check ---------
        #
        # Background for the two tests below. A TLS connection starts with a
        # "handshake": the server presents its certificate, the client presents
        # its own when asked, and each side checks the other's. That check is
        # where the cluster bus verifies that the connecting peer's certificate
        # carries the expected identity (tls-expected-peer-name). The handshake
        # is expensive, so TLS lets a client that has connected once come back
        # and "resume" the earlier session: the server hands out a token (a
        # session ID in TLS 1.2, a "ticket" in TLS 1.2 and 1.3), and a client
        # presenting that token later gets a short handshake in which NO
        # certificate is checked at all. OpenSSL simply restores the verdict it
        # stored with the original session.
        #
        # The node has two TLS listeners: the client port, where a client
        # certificate is required only as tls-auth-clients says, and the
        # cluster bus port, where one is always required and, here, must carry
        # the expected identity as well. A session obtained on the client port
        # must therefore never be accepted as a resumption on the bus, or those
        # checks are skipped. The bus accepts with a server context of its
        # own, so a client-port session is unknown there: not in its cache,
        # and sealed with a ticket key it does not have. The first test below
        # checks this; cluster-bus-tls.tcl does the same for a session
        # obtained without any certificate. Separately, the server tags the
        # sessions of every accepted connection with a "session ID context"
        # derived from the way that connection verifies (its
        # client-certificate requirement and its expected names), and OpenSSL
        # only resumes a session whose tag matches. That is what stops
        # resumption across a change of tls-expected-peer-name, which rebuilds
        # no context, as the second test below shows.
        #
        # The tests drive this with the openssl command line client, s_client,
        # through the openssl_s_client helper in tests/support/util.tcl. It
        # connects, performs the handshake, prints a report and exits. Two
        # lines of that report matter here: "New, TLSv1.x" means a full
        # handshake took place, "Reused, TLSv1.x" means the session was resumed.
        # With -sess_out FILE it saves the session token it received; with
        # -sess_in FILE it tries to resume from a saved token.

        test "A client-port TLS session cannot be resumed on the cluster bus" {
            # The attacker holds redis.crt: issued by the trusted CA, so it
            # passes the CA chain check, but carrying no identity, so the bus
            # rejects it in a full handshake (previous test). Here the attacker
            # tries to avoid the full handshake altogether.
            #
            # Repeated for both protocol versions, because they resume
            # differently: TLS 1.2 by session ID or ticket, TLS 1.3 by a
            # pre-shared key carried in the ticket. The fix must hold for both.
            set sess [file join [pwd] tests/tmp resumed.[pid].pem]
            foreach ver [tls_client_versions $target_host [srv 0 port]] {
                # Step 1: connect to the ordinary CLIENT port with the attacker
                # certificate. Only the CA chain is checked there, so the
                # handshake succeeds ("New") and s_client saves the session
                # token the server handed out.
                set out [openssl_s_client $target_host [srv 0 port] $ver \
                    -cert $::tlsdir/redis.crt -key $::tlsdir/redis.key -sess_out $sess]
                assert_match {*New, TLSv1*} $out
                assert {[file size $sess] > 0}

                # Step 2: connect to the CLUSTER BUS port presenting that token.
                # Without the fix the server would accept it as a resumption,
                # skip the certificate check, and the attacker would be on the
                # bus. Count the bus rejections logged so far, to detect a new
                # one below.
                set rejected [count_log_message 0 "Error accepting cluster node connection"]
                set out [openssl_s_client $target_host $target_bus_port $ver \
                    -cert $::tlsdir/redis.crt -key $::tlsdir/redis.key -sess_in $sess]

                # Step 3: the server must have refused to resume and run a full
                # handshake instead: s_client must not report "Reused" ...
                assert_no_match {*Reused, TLSv1*} $out
                # ... and that full handshake must have failed on the missing
                # identity, which the server logs as a rejected bus connection.
                wait_for_condition 50 100 {
                    [count_log_message 0 "Error accepting cluster node connection"] > $rejected
                } else {
                    fail "the bus did not run a full, verified handshake for the resumed session ($ver)"
                }
            }
            file delete $sess
        }

        test "A bus TLS session is not resumed after tls-expected-peer-name changes" {
            # The other way a resumption could skip a check: a legitimate member
            # connected while one identity policy was in force, and the policy
            # was then changed with CONFIG SET. Its old session must not be
            # resumed under the new policy, since nothing has verified the peer
            # against the new names yet. The member certificate san.crt carries
            # redis.local and cluster.local but not other.local, so the full
            # handshake that the change forces succeeds under the second name
            # and is refused under the third: the new policy is enforced, not
            # merely a handshake run.
            #
            # Every way a session can be resumed is tried, as in the bus test
            # of cluster-bus-tls.tcl, and the issuing connection is kept open
            # for the same reason: the server drops the cached session of a
            # connection its peer closed first.
            set host $target_host
            set bus $target_bus_port
            set member [list -cert $::tlsdir/san.crt -key $::tlsdir/san.key]
            set sess [file join [pwd] tests/tmp policy.[pid].pem]
            set variants {
                {-tls1_2}            TLSv1.2 ticket
                {-tls1_2 -no_ticket} TLSv1.2 id
            }
            if {[tls13_supported $host [srv 0 port]]} {
                set variants [concat {{-tls1_3} TLSv1.3 ticket} $variants]
            }
            foreach {flags proto by} $variants {
                # Step 1: a full handshake on the bus under redis.local, saving
                # the session, which has to be of the kind this variant resumes
                # by.
                file delete $sess
                set issuing [openssl_s_client_open $host $bus {*}$flags {*}$member -sess_out $sess]
                wait_for_condition 50 100 {
                    [file exists $sess] && ![catch {exec openssl sess_id -in $sess -noout}]
                } else {
                    fail "no session was saved from the bus"
                }
                set saved [exec openssl sess_id -in $sess -noout -text 2>@1]
                assert_match "*Protocol*: $proto*" $saved
                if {$by eq "ticket"} {
                    assert_match {*TLS session ticket:*} $saved
                } else {
                    assert_no_match {*TLS session ticket:*} $saved
                    assert {[regexp {Session-ID: [0-9A-F]+} $saved]}
                }

                # Step 2: a name the certificate also carries. The session is
                # not resumed; the full handshake run instead verifies the
                # peer against the new name and succeeds.
                R 0 config set tls-expected-peer-name cluster.local
                set out [openssl_s_client $host $bus {*}$flags {*}$member -sess_in $sess]
                assert_no_match {*Reused, TLSv1*} $out
                assert_match {*New, TLSv1*} $out
                assert_no_match {*alert number*} $out

                # Step 3: a name the certificate does not carry. Not resumed
                # either, and the full handshake is refused, which the bus
                # logs as a rejected connection.
                R 0 config set tls-expected-peer-name other.local
                set loglines [count_log_lines 0]
                set out [openssl_s_client $host $bus {*}$flags {*}$member -sess_in $sess]
                assert_no_match {*Reused, TLSv1*} $out
                wait_for_log_messages 0 {"*Error accepting cluster node connection*"} $loglines 50 100

                # Positive control: back under the issuing policy the session
                # resumes, so the refusals above were the policy change at
                # work, not a session that had become unusable.
                R 0 config set tls-expected-peer-name redis.local
                set out [openssl_s_client $host $bus {*}$flags {*}$member -sess_in $sess]
                assert_match {*Reused, TLSv1*} $out

                assert_match {*New, TLSv1*} [openssl_s_client_close $issuing]
            }
            file delete $sess
        }

        test "The same forged FAIL is processed when the cert carries the SAN (positive control)" {
            # Positive control for the previous test: it proves the forged packet is
            # well-formed and acted upon, and that "FAIL message received" is the
            # line logged when it is, so the absence asserted above is meaningful.
            # The packet is replayed byte for byte; only the certificate differs:
            # san.crt carries redis.local, so the accept-side check passes and the
            # forged FAIL about node 2 is processed exactly as the threat model
            # describes.
            #
            # Node 2 is a master with slots, so node 0 would keep the FAIL flag for
            # twice cluster-node-timeout (6 s here) before trusting it again. Lower
            # the timeout on node 0 alone for the duration of this test so the
            # cluster is restored well within a second. Node 0 by itself cannot
            # reach the quorum needed to fail anyone for real, so the other nodes,
            # which keep their timeout, are unaffected.
            set old_timeout [lindex [R 0 CONFIG GET cluster-node-timeout] 1]
            R 0 CONFIG SET cluster-node-timeout 300

            # Keep the socket open until the packet has been handled, so the close
            # cannot race the read.
            set fd [::tls::socket \
                -cafile "$::tlsdir/ca.crt" \
                -certfile "$::tlsdir/san.crt" \
                -keyfile "$::tlsdir/san.key" \
                $target_host $target_bus_port]
            fconfigure $fd -translation binary -buffering full
            puts -nonewline $fd $packet
            flush $fd
            wait_for_log_messages 0 \
                [list "*FAIL message received from ${spoofed_sender}*about ${failed_target}*"] 0 50 100
            close $fd

            # Node 0 flagged node 2 as failed on that line; wait for it to clear the
            # flag again, then leave the cluster as we found it.
            wait_for_log_messages 0 [list "*Clear FAIL state for node ${failed_target}*"] 0 50 100
            R 0 CONFIG SET cluster-node-timeout $old_timeout
            assert_equal 0 [cluster_has_flag [cluster_node_view 0 $failed_target] fail]
            wait_for_cluster_state ok
        }
    }
}
