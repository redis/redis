# Certificate trust on the cluster bus, which is what "tls-cluster yes" buys: a
# peer has to present a certificate that verifies against the configured CA, in
# the accepting and in the connecting direction alike, and neither direction can
# be relaxed through tls-auth-clients, nor sidestepped by resuming a TLS session
# obtained on the client port.
#
# These properties are what cluster-bus-port-protected-mode relies on when it
# treats tls-cluster on its own as an authenticated bus port, but they belong to
# tls-cluster and are pinned here independently of that option.

# Opens a cluster bus connection presenting the given client certificate, then
# closes it. The handshake outcome is deliberately not inspected here: under TLS
# 1.3 the client's own handshake completes before the server has validated the
# certificate, so a rejection is only visible in the server's log. Callers assert
# on that.
proc bus_connect_with_cert {host port crt key} {
    catch {
        set fd [::tls::socket -cafile "$::tlsdir/ca.crt" -certfile $crt -keyfile $key $host $port]
        ::tls::handshake $fd
        close $fd
    }
}

if {$::tls} {
    start_cluster 1 0 {tags {external:skip cluster tls}} {
        test {a cluster bus peer whose certificate does not chain to the CA is rejected} {
            # A self-signed certificate is valid TLS material with no path to
            # tls-ca-cert-file, and has to be refused on the very port that
            # accepts the suite's CA-signed one.
            set dir [file normalize [tmpdir cluster-bus-untrusted-ca]]
            exec -ignorestderr openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
                -subj "/O=Rogue/CN=rogue.example" \
                -keyout $dir/rogue.key -out $dir/rogue.crt 2>/dev/null

            set host [srv 0 host]
            set bus [expr {[srv 0 port] + 10000}]
            set rejection "Error accepting cluster node connection"
            set before [count_log_message 0 $rejection]
            set loglines [count_log_lines 0]

            bus_connect_with_cert $host $bus $::tlsdir/redis.crt $::tlsdir/redis.key
            bus_connect_with_cert $host $bus $dir/rogue.crt $dir/rogue.key

            wait_for_log_messages 0 \
                {"*Error accepting cluster node connection*certificate verify failed*"} \
                $loglines 50 100
            # Exactly one rejection, so the CA-signed peer was accepted on the
            # same port rather than everything being refused.
            assert_equal [expr {$before + 1}] [count_log_message 0 $rejection]
        }

        test {a cluster bus peer whose server certificate does not chain to the CA is refused} {
            # The dialling side verifies too: an outbound bus link uses the
            # client-role context, which loads the same CA, so a peer serving
            # a certificate that does not chain to it must be refused and
            # never admitted to the cluster.
            set dir [file normalize [tmpdir cluster-bus-untrusted-peer]]
            exec -ignorestderr openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
                -subj "/O=Rogue/CN=rogue.example" \
                -keyout $dir/rogue.key -out $dir/rogue.crt 2>/dev/null

            # A second cluster node, identical to this one except that it serves
            # a self-signed certificate. "wait_ready false" because the suite's
            # own client would refuse that certificate as well, so no client is
            # created for it; inside this block R 1 is the node under test and
            # srv 0 the untrusted peer.
            start_server [list wait_ready false overrides [list cluster-enabled yes \
                    tls-cert-file $dir/rogue.crt tls-key-file $dir/rogue.key \
                    tls-client-cert-file $dir/rogue.crt tls-client-key-file $dir/rogue.key]] {
                wait_for_condition 50 100 {
                    [count_message_lines [srv 0 stdout] "Ready to accept"] > 0
                } else {
                    fail "the peer serving an untrusted certificate did not start"
                }

                # Commands use R's positive indexing, the log helpers take srv's
                # negative one; both mean the node under test here.
                set rogue_bus [expr {[srv 0 port] + 10000}]
                set loglines [count_log_lines -1]
                R 1 cluster meet [srv 0 host] [srv 0 port]

                wait_for_log_messages -1 \
                    [list "*at [srv 0 host]:$rogue_bus failed*certificate verify failed*"] \
                    $loglines 50 100

                # And it is never admitted: the handshake node is dropped once it
                # expires, leaving this node on its own again.
                wait_for_condition 50 100 {
                    [llength [get_cluster_nodes 1]] == 1
                } else {
                    fail "the peer serving an untrusted certificate was admitted"
                }
            }
        }
    }

    # tls-auth-clients governs the client port only. Pin that with the setting
    # that would weaken the bus if it could reach it.
    start_cluster 1 0 {tags {external:skip cluster tls} overrides {tls-auth-clients no}} {
        test {tls-auth-clients does not relax the cluster bus} {
            set host [srv 0 host]
            set bus [expr {[srv 0 port] + 10000}]

            # The setting is in effect: on the client port a peer presenting no
            # certificate is served. Driven through redis-cli, since the suite's
            # own client always offers one.
            assert_equal {PONG} [string trim [exec src/redis-cli --tls \
                --cacert "$::tlsdir/ca.crt" -h $host -p [srv 0 port] ping]]

            # The cluster bus is unaffected: clusterAcceptHandler() demands a
            # certificate whatever tls-auth-clients says, so the same peer is
            # refused there. Read from the log, as a TLS 1.3 client is not told:
            # its own handshake completes before the server judges it.
            set loglines [count_log_lines 0]
            catch {exec openssl s_client -connect $host:$bus \
                -CAfile "$::tlsdir/ca.crt" << "" 2> /dev/null}
            wait_for_log_messages 0 \
                {"*Error accepting cluster node connection*peer did not return a certificate*"} \
                $loglines 50 100
        }

        test {a certificate-less client-port session cannot be resumed on the cluster bus} {
            # The certificate requirement is enforced in a full handshake only.
            # A resumed session skips the certificate exchange and restores the
            # verdict stored with the original session, so a session obtained
            # on the client port without any certificate must never be accepted
            # as a resumption on the bus: anyone able to reach both ports would
            # otherwise be on the bus with no credential at all. The bus accepts
            # with a server context of its own, so a client-port session is not
            # in its cache and its ticket cannot be decrypted there. Each
            # accepted connection's sessions also carry a context derived from
            # the way it verifies, and OpenSSL resumes only on a matching one.
            #
            # Two client-port settings serve a client that presents no
            # certificate: "no" never asks for one, "optional" asks but does
            # not insist. The test sets each itself, as the setting is read
            # when a connection is accepted and so applies to the very next
            # one. Every way a session can be resumed is tried, since they take
            # different paths in OpenSSL: TLS 1.2 by a ticket or, with
            # -no_ticket, by a session ID looked up in the server's cache;
            # TLS 1.3, where the server has it, by a pre-shared key carried in
            # the ticket. The cache path needs session caching on, so the test
            # turns it on itself.
            #
            # The connection that obtains each session is kept open until the
            # attempts are over. OpenSSL drops a cached session when its
            # connection is freed without a close_notify having been sent, and
            # the server sends none for a connection its peer closed first, so
            # a session presented after its connection had gone would be
            # refused for that reason alone, fix or no fix.
            set host [srv 0 host]
            set port [srv 0 port]
            set bus [expr {$port + 10000}]
            set sess [file join [pwd] tests/tmp nocert.[pid].pem]
            set old_auth [lindex [R 0 config get tls-auth-clients] 1]
            set old_caching [lindex [R 0 config get tls-session-caching] 1]
            R 0 config set tls-session-caching yes
            foreach mode {no optional} {
                R 0 config set tls-auth-clients $mode
                set variants {
                    {-tls1_2}            TLSv1.2 ticket
                    {-tls1_2 -no_ticket} TLSv1.2 id
                }
                if {[tls13_supported $host $port]} {
                    set variants [concat {{-tls1_3} TLSv1.3 ticket} $variants]
                }
                foreach {flags proto by} $variants {
                    # A full handshake on the client port, presenting no
                    # certificate, saving the session the server hands out. The
                    # saved session has to be the one just negotiated, and
                    # resumable the way this variant means to resume it.
                    file delete $sess
                    set issuing [openssl_s_client_open $host $port {*}$flags -sess_out $sess]
                    wait_for_condition 50 100 {
                        [file exists $sess] && ![catch {exec openssl sess_id -in $sess -noout}]
                    } else {
                        fail "no session was saved from the client port"
                    }
                    set saved [exec openssl sess_id -in $sess -noout -text 2>@1]
                    assert_match "*Protocol*: $proto*" $saved
                    if {$by eq "ticket"} {
                        assert_match {*TLS session ticket:*} $saved
                    } else {
                        assert_no_match {*TLS session ticket:*} $saved
                        assert {[regexp {Session-ID: [0-9A-F]+} $saved]}
                    }

                    # Present it on the bus. No resumption may be reported, and
                    # the full handshake that runs instead has to be refused
                    # for the missing certificate, which only the log shows.
                    set loglines [count_log_lines 0]
                    set out [openssl_s_client $host $bus {*}$flags -sess_in $sess]
                    assert_no_match {*Reused, TLSv1*} $out
                    wait_for_log_messages 0 \
                        {"*Error accepting cluster node connection*peer did not return a certificate*"} \
                        $loglines 50 100

                    # Positive control: the same session does resume where the
                    # policy matches, so the refusal above is the context check
                    # at work, not a session that could not be resumed at all.
                    set out [openssl_s_client $host $port {*}$flags -sess_in $sess]
                    assert_match {*Reused, TLSv1*} $out

                    # Release the issuing connection, held open until now. Its
                    # output is a sanity check only: the handshake the session
                    # came from was a full one and the client exited normally.
                    assert_match {*New, TLSv1*} [openssl_s_client_close $issuing]
                }
            }
            R 0 config set tls-auth-clients $old_auth
            R 0 config set tls-session-caching $old_caching
            file delete $sess
        }
    }
}
