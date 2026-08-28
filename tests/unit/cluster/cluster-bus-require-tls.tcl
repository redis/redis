# cluster-bus-require-tls makes a plaintext cluster bus an explicit choice.
#
# The cluster bus has no authentication of its own (a packet's sender is
# identified only by the public node ID in its header) and it carries sensitive
# information shared between the nodes, which must not leak outside the cluster.
# So anyone able to reach or eavesdrop on the cluster bus port of a plaintext bus
# can potentially threaten the whole cluster. The option defaults to yes and
# refuses such a setup, in the spirit of protected-mode; the operator waives it with
# "cluster-bus-require-tls no" once the port is known to be firewalled off.
#
# Note that the suite's default.conf waives the requirement for every server it
# starts (non-TLS runs have no TLS to offer), so the tests below that care about
# the default state must set the option, or bypass default.conf altogether.

tags {external:skip cluster} {

    test {cluster-bus-require-tls defaults to yes and refuses a plaintext cluster bus} {
        # No config file at all: this observes the compiled-in default rather
        # than anything the suite configures. "--daemonize yes" only matters if
        # this check ever regresses: the server would then start instead of
        # failing, and exec would return at once instead of blocking forever.
        catch {exec src/redis-server --cluster-enabled yes --port 0 --daemonize yes} err
        assert_match {*FATAL CONFIG FILE ERROR*} $err
        assert_match {*cluster-bus-require-tls is enabled but tls-cluster is disabled*} $err
        assert_match {*'cluster-bus-require-tls no'*} $err
    }

    # A standalone instance opens no cluster bus, so the default requirement must
    # not stop it from starting with plain TCP everywhere.
    start_server {overrides {cluster-bus-require-tls yes tls-cluster no}} {
        test {cluster-bus-require-tls only applies to cluster mode} {
            assert_equal {cluster-bus-require-tls yes} [r config get cluster-bus-require-tls]
            assert_equal {tls-cluster no} [r config get tls-cluster]
            assert_equal 0 [s cluster_enabled]
            assert_equal {PONG} [r ping]
            # Outside cluster mode the two options are unrelated, so both stay
            # freely settable.
            r config set cluster-bus-require-tls no
            r config set cluster-bus-require-tls yes
            assert_equal 0 [count_log_message 0 "cluster bus is not protected by TLS"]
        }

        test {the cluster-mode gate of the requirement cannot be opened at runtime} {
            # Gating the requirement on cluster mode is only sound because
            # cluster-enabled is IMMUTABLE_CONFIG. Could it be set, this very
            # server - requirement enabled, tls-cluster off - would open a
            # plaintext cluster bus without ever facing the startup check. The
            # immutable mechanism itself is covered by "CONFIG SET set immutable"
            # in unit/introspection; what is pinned here is that cluster-enabled
            # in particular still carries the flag.
            assert_error {*can't set immutable config*} {r config set cluster-enabled yes}
            assert_equal 0 [s cluster_enabled]
        }
    }

    start_cluster 1 0 {overrides {cluster-bus-require-tls no tls-cluster no}} {
        test {waiving the requirement starts a cluster node with a plaintext bus} {
            assert_equal 1 [s cluster_enabled]
            assert_equal {tls-cluster no} [r config get tls-cluster]
            wait_for_cluster_state ok
        }

        test {a waived requirement is reported in the log} {
            verify_log_message 0 "*cluster bus is not protected by TLS*" 0
        }

        test {CONFIG SET cluster-bus-require-tls yes is refused while the bus is plaintext} {
            assert_error {*can't enable cluster-bus-require-tls while tls-cluster is disabled*} {
                r config set cluster-bus-require-tls yes
            }
            assert_equal {cluster-bus-require-tls no} [r config get cluster-bus-require-tls]
        }
    }

    # The other direction, and the atomic transitions between the two states,
    # need a cluster bus that can actually run on TLS.
    if {$::tls} {
        start_cluster 1 0 {overrides {cluster-bus-require-tls yes}} {
            test {a cluster node starts with the requirement met by tls-cluster} {
                assert_equal 1 [s cluster_enabled]
                assert_equal {tls-cluster yes} [r config get tls-cluster]
                wait_for_cluster_state ok
                assert_equal 0 [count_log_message 0 "cluster bus is not protected by TLS"]
            }

            test {CONFIG SET tls-cluster no is refused while the requirement is enabled} {
                assert_error {*can't disable tls-cluster while cluster-bus-require-tls is enabled*} {
                    r config set tls-cluster no
                }
                assert_equal {tls-cluster yes} [r config get tls-cluster]
                assert_equal 0 [count_log_message 0 "cluster bus is not protected by TLS"]
            }

            test {a single CONFIG SET can move the bus to plain TCP, in either argument order} {
                # Every setter of a CONFIG SET runs before the first apply
                # callback, so the pair is judged on the state it produces and
                # not on the order it is written in.
                foreach args {{cluster-bus-require-tls no tls-cluster no}
                              {tls-cluster no cluster-bus-require-tls no}} {
                    set logged [count_log_message 0 "cluster bus is not protected by TLS"]
                    r config set {*}$args
                    assert_equal {tls-cluster no} [r config get tls-cluster]
                    assert_equal {cluster-bus-require-tls no} [r config get cluster-bus-require-tls]
                    # Leaving TLS is reported once, as it is at startup.
                    assert_equal [expr {$logged + 1}] [count_log_message 0 "cluster bus is not protected by TLS"]

                    # And back: enabling the requirement again needs TLS back on.
                    r config set tls-cluster yes cluster-bus-require-tls yes
                    assert_equal {tls-cluster yes} [r config get tls-cluster]
                    assert_equal {cluster-bus-require-tls yes} [r config get cluster-bus-require-tls]
                }
            }
        }
    }
}
