# Certificate trust on the cluster bus, which is what "tls-cluster yes" buys: a
# peer has to present a certificate that verifies against the configured CA, in
# the accepting and in the connecting direction alike, and neither direction can
# be relaxed through tls-auth-clients.
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
    }
}
