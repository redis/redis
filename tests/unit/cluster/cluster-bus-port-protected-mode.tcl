# cluster-bus-port-protected-mode makes an unauthenticated cluster bus port an
# explicit choice.
#
# The cluster bus port has no authentication of its own: any host able to reach it
# can speak the cluster protocol and potentially threaten the whole cluster - a
# MEET from an unknown host is enough to have it added as a node, with its gossip
# trusted. What authenticates it is tls-cluster, which makes every peer present a
# certificate verified against the CA. The option defaults to yes and refuses an
# unauthenticated bus, in the spirit of protected-mode; the operator waives it
# with "cluster-bus-port-protected-mode no" once the port is known to be
# firewalled off.
#
# Note that the suite's default.conf waives protection for every server it
# starts (non-TLS runs have no TLS to offer), so the tests below that care about
# the default state must set the option, or bypass default.conf altogether.

# Writes a two-file configuration into $dir: the given directives go to
# inner.conf, which main.conf pulls in with "include" before setting the port and
# the directory. Anything in inner.conf is therefore parsed before the rest of
# main.conf, which is what the include test below needs to exercise.
proc write_included_conf {dir port inner_lines} {
    set fd [open $dir/inner.conf w]
    foreach line $inner_lines { puts $fd $line }
    close $fd

    set fd [open $dir/main.conf w]
    puts $fd "include $dir/inner.conf"
    puts $fd "port $port"
    puts $fd "dir $dir"
    close $fd
}

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

tags {external:skip cluster} {

    test {cluster-bus-port-protected-mode defaults to yes and refuses an unauthenticated bus} {
        # No config file at all: this observes the compiled-in default rather
        # than anything the suite configures. "--daemonize yes" only matters if
        # this check ever regresses: the server would then start instead of
        # failing, and exec would return at once instead of blocking forever.
        catch {exec src/redis-server --cluster-enabled yes --port 0 --daemonize yes} err
        assert_match {*FATAL CONFIG FILE ERROR*} $err
        assert_match {*cluster-bus-port-protected-mode is enabled but tls-cluster is disabled*} $err
        assert_match {*'cluster-bus-port-protected-mode no'*} $err
    }

    test {protection is judged after the whole configuration is loaded} {
        # The check runs when the OUTERMOST config load finishes, so directives
        # that reach the server through "include" are accounted for. This matters
        # because an included file ends at the position of its "include" line,
        # before the rest of the parent file has been parsed. start_server cannot
        # be used here: it generates the config file itself and offers no way to
        # place directives behind an include.
        set dir [file normalize [tmpdir cluster-bus-include]]
        set logf $dir/redis.log
        set port [find_available_port $::baseport $::portcount]

        # cluster mode arrives via the include: the node does open a cluster bus,
        # so the default protection must still refuse an unauthenticated one.
        write_included_conf $dir $port {"cluster-enabled yes"}
        catch {exec src/redis-server $dir/main.conf --daemonize yes} err
        assert_match {*cluster-bus-port-protected-mode is enabled but tls-cluster is disabled*} $err

        # And the waiver arrives via the include too: it is honoured, and the
        # node starts with an unauthenticated bus.
        write_included_conf $dir $port {"cluster-enabled yes" "cluster-bus-port-protected-mode no"}
        exec src/redis-server $dir/main.conf --daemonize yes --logfile $logf
        wait_for_condition 100 100 {
            [count_message_lines $logf "Ready to accept"] > 0
        } else {
            fail "Server did not start; log: $logf"
        }
        # Connect over the plain port: this server knows nothing about the
        # suite's TLS configuration, in a --tls run just as much as without it.
        set rd [redis 127.0.0.1 $port 0 0]
        assert_equal {cluster-bus-port-protected-mode no} [$rd config get cluster-bus-port-protected-mode]
        assert_equal 1 [status $rd cluster_enabled]
        assert_equal 1 [count_message_lines $logf "cluster bus port is not authenticated"]
        catch {$rd shutdown nosave}
        $rd close
    }

    # A standalone instance opens no cluster bus port, so the default protection
    # must not stop it from starting without any TLS at all.
    start_server {overrides {cluster-bus-port-protected-mode yes tls-cluster no}} {
        test {cluster-bus-port-protected-mode only applies to cluster mode} {
            assert_equal {cluster-bus-port-protected-mode yes} [r config get cluster-bus-port-protected-mode]
            assert_equal {tls-cluster no} [r config get tls-cluster]
            assert_equal 0 [s cluster_enabled]
            assert_equal {PONG} [r ping]
            # Outside cluster mode the two options are unrelated, so both stay
            # freely settable.
            r config set cluster-bus-port-protected-mode no
            r config set cluster-bus-port-protected-mode yes
            assert_equal 0 [count_log_message 0 "cluster bus port is not authenticated"]
        }

        test {the cluster-mode gate of protection cannot be opened at runtime} {
            # Gating protection on cluster mode is only sound because
            # cluster-enabled is IMMUTABLE_CONFIG. Could it be set, this very
            # server - protection enabled, tls-cluster off - would open an
            # unauthenticated cluster bus without ever facing the startup check. The
            # immutable mechanism itself is covered by "CONFIG SET set immutable"
            # in unit/introspection; what is pinned here is that cluster-enabled
            # in particular still carries the flag.
            assert_error {*can't set immutable config*} {r config set cluster-enabled yes}
            assert_equal 0 [s cluster_enabled]
        }
    }

    start_cluster 1 0 {overrides {cluster-bus-port-protected-mode no tls-cluster no}} {
        test {waiving protection starts a cluster node with an unauthenticated bus} {
            assert_equal 1 [s cluster_enabled]
            assert_equal {tls-cluster no} [r config get tls-cluster]
            wait_for_cluster_state ok
        }

        test {waived protection is reported in the log} {
            verify_log_message 0 "*cluster bus port is not authenticated*" 0
        }

        test {CONFIG SET cluster-bus-port-protected-mode yes is refused while the bus is unauthenticated} {
            assert_error {*can't enable cluster-bus-port-protected-mode while tls-cluster is disabled*} {
                r config set cluster-bus-port-protected-mode yes
            }
            assert_equal {cluster-bus-port-protected-mode no} [r config get cluster-bus-port-protected-mode]
        }
    }

    # The other direction, and the atomic transitions between the two states,
    # need a cluster bus that can actually run on TLS.
    if {$::tls} {
        start_cluster 1 0 {overrides {cluster-bus-port-protected-mode yes}} {
            test {a cluster node starts with its bus authenticated by tls-cluster} {
                assert_equal 1 [s cluster_enabled]
                assert_equal {tls-cluster yes} [r config get tls-cluster]
                wait_for_cluster_state ok
                assert_equal 0 [count_log_message 0 "cluster bus port is not authenticated"]
            }

            test {a cluster bus peer whose certificate does not chain to the CA is rejected} {
                # What protected mode buys is authentication of the bus port, so
                # pin what that authentication actually rejects. A self-signed
                # certificate is valid TLS material with no path to
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

                # A second cluster node, identical to this one except that it
                # serves a self-signed certificate. "wait_ready false" because the
                # suite's own client would refuse that certificate as well, so no
                # client is created for it; inside this block R 1 is the node
                # under test and srv 0 the untrusted peer.
                start_server [list wait_ready false overrides [list cluster-enabled yes \
                        tls-cert-file $dir/rogue.crt tls-key-file $dir/rogue.key \
                        tls-client-cert-file $dir/rogue.crt tls-client-key-file $dir/rogue.key]] {
                    wait_for_condition 50 100 {
                        [count_message_lines [srv 0 stdout] "Ready to accept"] > 0
                    } else {
                        fail "the peer serving an untrusted certificate did not start"
                    }

                    # Commands use R's positive indexing, the log helpers take
                    # srv's negative one; both mean the node under test here.
                    set rogue_bus [expr {[srv 0 port] + 10000}]
                    set loglines [count_log_lines -1]
                    R 1 cluster meet [srv 0 host] [srv 0 port]

                    wait_for_log_messages -1 \
                        [list "*at [srv 0 host]:$rogue_bus failed*certificate verify failed*"] \
                        $loglines 50 100

                    # And it is never admitted: the handshake node is dropped once
                    # it expires, leaving this node on its own again.
                    wait_for_condition 50 100 {
                        [llength [get_cluster_nodes 1]] == 1
                    } else {
                        fail "the peer serving an untrusted certificate was admitted"
                    }
                }
            }

            test {CONFIG SET tls-cluster no is refused while protected mode is enabled} {
                assert_error {*can't disable tls-cluster while cluster-bus-port-protected-mode is enabled*} {
                    r config set tls-cluster no
                }
                assert_equal {tls-cluster yes} [r config get tls-cluster]
                assert_equal 0 [count_log_message 0 "cluster bus port is not authenticated"]
            }

            test {a single CONFIG SET can un-authenticate the bus, in either argument order} {
                # Every setter of a CONFIG SET runs before the first apply
                # callback, so the pair is judged on the state it produces and
                # not on the order it is written in.
                foreach args {{cluster-bus-port-protected-mode no tls-cluster no}
                              {tls-cluster no cluster-bus-port-protected-mode no}} {
                    set logged [count_log_message 0 "cluster bus port is not authenticated"]
                    r config set {*}$args
                    assert_equal {tls-cluster no} [r config get tls-cluster]
                    assert_equal {cluster-bus-port-protected-mode no} [r config get cluster-bus-port-protected-mode]
                    # Leaving TLS is reported once, as it is at startup.
                    assert_equal [expr {$logged + 1}] [count_log_message 0 "cluster bus port is not authenticated"]

                    # And back: enabling protection again needs TLS back on.
                    r config set tls-cluster yes cluster-bus-port-protected-mode yes
                    assert_equal {tls-cluster yes} [r config get tls-cluster]
                    assert_equal {cluster-bus-port-protected-mode yes} [r config get cluster-bus-port-protected-mode]
                }
            }

            test {each option can also be changed on its own, in the safe order} {
                # A single command is not the only way out, and the refusals say
                # so: waiving protection before disabling tls-cluster, and
                # authenticating the bus before enabling protection, are accepted
                # as separate calls, since the check only judges the state each
                # one produces.
                r config set cluster-bus-port-protected-mode no
                r config set tls-cluster no
                assert_equal {tls-cluster no} [r config get tls-cluster]

                r config set tls-cluster yes
                r config set cluster-bus-port-protected-mode yes
                assert_equal {cluster-bus-port-protected-mode yes} [r config get cluster-bus-port-protected-mode]
            }
        }
    }
}
