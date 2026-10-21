#
# Copyright (c) 2026-Present, Redis Ltd.
# All rights reserved.
#
# Licensed under your choice of (a) the Redis Source Available License 2.0
# (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
# GNU Affero General Public License v3 (AGPLv3).
#

proc compression_client_io_thread {info} {
    assert {[regexp {io-thread=(\d+)} $info - tid]}
    return $tid
}

# Identify the replica by its listening port so connection order does not matter.
proc compression_replica_io_thread {master port} {
    foreach line [split [$master info replication] "\r\n"] {
        if {[string match "slave*:*,port=$port,*" $line]} {
            return [compression_client_io_thread $line]
        }
    }
    return -1
}

start_server {tags {repl iothreads external:skip} overrides {io-threads 4} omit {io-threads-client-types repl-compression repl-compression-max-latency}} {
    test {IO thread client types default to all and are immutable} {
        assert_equal {io-threads-client-types all} [r config get io-threads-client-types]
        assert {[compression_client_io_thread [r client info]] > 0}
        assert_error {*immutable config*} {r config set io-threads-client-types normal}
    }

    test {IO thread client types reject unknown selectors} {
        catch {exec src/redis-server --port 0 --io-threads-client-types unknown} err
        assert_match {*io-threads-client-types*unknown*argument(s) must be one of the following*} $err
    }

    test {Compression-only IO thread selectors require enabled compression} {
        foreach client_types {
            replica-compressed
            master-compressed
            replication-compressed
            {normal replica-compressed}
            {replica master-compressed}
        } {
            catch {exec src/redis-server --port 0 --io-threads 4 --io-threads-client-types $client_types} err
            assert_match {*io-threads-client-types compression-only selectors require compression support and repl-compression > 0*} $err
        }
    }

    if {$::compression} {
        test {Compression-only IO thread selector validation is independent of argument order} {
            foreach options {
                {--io-threads-client-types replication-compressed --repl-compression 0}
                {--repl-compression 0 --io-threads-client-types replication-compressed}
            } {
                catch {exec src/redis-server --port 0 --io-threads 4 {*}$options} err
                assert_match {*io-threads-client-types compression-only selectors require compression support and repl-compression > 0*} $err
            }
        }

        test {Replication compression requires an eligible replication client type} {
            foreach options {
                {--io-threads-client-types normal --repl-compression 1}
                {--repl-compression 1 --io-threads-client-types normal}
            } {
                catch {exec src/redis-server --port 0 --io-threads 4 {*}$options} err
                assert_match {*repl-compression requires io-threads-client-types to include replication clients*} $err
            }
        }
    }
}

# Generic role selectors include both plain and compressed connections.
# Compression-only selectors require a compression build and repl-compression > 0.
set selector_cases {
    0 normal normal                                               1 1 0 0 0
    0 replica replica                                             0 0 1 0 0
    0 master master                                             0 0 0 1 0
    0 replication replication                                     0 0 1 1 0
    0 all all                                                     1 1 1 1 0
    0 {replica master} {replica master}                           0 0 1 1 0
    0 {normal replica} {normal master}                            1 1 1 1 0
    0 {replica replica-compressed} {master master-compressed}   0 0 1 1 0
}
if {$::compression} {
    lappend selector_cases \
        1 replica all                                             0 1 1 1 1 \
        1 all master                                             1 0 1 1 1 \
        1 replication replication                                 0 0 1 1 1 \
        1 replica-compressed all                                 0 1 1 1 1 \
        1 all master-compressed                                 1 0 1 1 1 \
        1 replication-compressed replication-compressed         0 0 1 1 1 \
        1 all all                                                 1 1 1 1 1 \
        1 {replica-compressed master-compressed} {replica-compressed master-compressed} 0 0 1 1 1 \
        1 {normal replica master-compressed} {normal replica master-compressed} 1 1 1 1 1 \
        1 replica replica                                         0 0 1 0 0 \
        1 master master                                         0 0 0 1 0
}
foreach {compression_level master_types replica_types master_normal_worker replica_normal_worker replica_worker primary_worker compressed} $selector_cases {
    set overrides [list io-threads 4 io-threads-client-types $master_types save ""]
    set omit {repl-compression repl-compression-max-latency}
    if {$compression_level} {
        lappend overrides repl-compression $compression_level repl-compression-max-latency 10
        set omit {}
    }
    set context "master-types=$master_types replica-types=$replica_types compression=$compression_level"

    start_server [list tags {repl iothreads external:skip} overrides $overrides omit $omit] {
        start_server [list overrides [concat $overrides [list io-threads-client-types $replica_types]] omit $omit] {
            test "IO thread client type selection: $context" {
                set master [srv -1 client]
                set replica [srv 0 client]

                # Individual selectors and their bitwise aliases must be
                # accepted by the configuration parser in every build.
                foreach client [list $master $replica] client_types [list $master_types $replica_types] {
                    set expected_types $client_types
                    if {$client_types eq "replica master"} {
                        set expected_types replication
                    } elseif {$client_types eq "replica-compressed master-compressed"} {
                        set expected_types replication-compressed
                    }
                    if {[llength $client_types] == 1 || $expected_types ne $client_types} {
                        assert_equal $expected_types [lindex [$client config get io-threads-client-types] 1]
                    }
                    # A selector never creates more than the configured workers.
                    assert_equal -1 [get_io_thread_clients 4 $client]
                }

                $master set initial [string repeat initial 1000]
                $replica replicaof [srv -1 host] [srv -1 port]
                wait_for_sync $replica
                wait_replica_online $master
                $master set streamed [string repeat streamed 1000]
                assert_equal 1 [$master wait 1 5000]
                wait_for_ofs_sync $master $replica
                # Thread selection must preserve the replicated dataset.
                assert_equal [$master debug digest] [$replica debug digest]

                wait_for_condition 50 100 {
                    ([compression_client_io_thread [$master client list type replica]] > 0) == $replica_worker &&
                    ([compression_client_io_thread [$replica client list type master]] > 0) == $primary_worker
                } else {
                    fail "Unexpected replication thread assignment: $context"
                }
                foreach client [list $master $replica] normal_worker [list $master_normal_worker $replica_normal_worker] {
                    # Normal command connections follow only the normal selector.
                    assert_equal $normal_worker [expr {[compression_client_io_thread [$client client info]] > 0}]
                }
                if {$compressed} {
                    # These links negotiated compression and use eligible workers.
                    assert {[status $master total_net_repl_uncompressed_bytes] > 0}
                    assert {[status $replica total_net_repl_decompressed_bytes] > 0}
                } elseif {$compression_level} {
                    # A direction excluded by its selector must decline compression.
                    assert_equal {} [status $master total_net_repl_uncompressed_bytes]
                    assert_equal {} [status $replica total_net_repl_decompressed_bytes]
                }

                # A reconnect must apply the same role filter after PSYNC.
                set partial_syncs [status $master sync_partial_ok]
                $replica client kill type master
                $master set reconnected [string repeat reconnected 1000]
                wait_for_condition 100 100 {
                    [status $master sync_partial_ok] > $partial_syncs &&
                    [$replica get reconnected] eq [string repeat reconnected 1000]
                } else {
                    fail "Replication did not resume via partial sync: $context"
                }
                assert_equal 1 [$master wait 1 5000]
                wait_for_condition 50 100 {
                    ([compression_client_io_thread [$master client list type replica]] > 0) == $replica_worker &&
                    ([compression_client_io_thread [$replica client list type master]] > 0) == $primary_worker
                } else {
                    fail "Unexpected replication thread assignment after partial sync: $context"
                }
            }
        }
    }
}

start_server {tags {repl iothreads external:skip} overrides {io-threads 1 io-threads-client-types all} omit {repl-compression repl-compression-max-latency}} {
    test {IO thread client types do not create workers when io-threads is one} {
        assert_equal 1 [lindex [r config get io-threads] 1]
        assert_equal -1 [get_io_thread_clients 1]
        assert_equal 0 [compression_client_io_thread [r client info]]
    }
}

# The remaining tests configure repl-compression, which is only registered in
# builds with BUILD_COMPRESSION=yes. The compression CI job passes --compression.
if {!$::compression} {
    return
}

start_server {tags {repl iothreads external:skip} overrides {io-threads 4 io-threads-client-types replication repl-compression 1 save ""}} {
    start_server {overrides {io-threads 4 io-threads-client-types replication repl-compression 2 save ""}} {
        test {IO thread client types follow uncompressed fallback after compression negotiation fails} {
            set master [srv -1 client]
            set replica [srv 0 client]
            $replica replicaof [srv -1 host] [srv -1 port]
            wait_for_sync $replica
            wait_replica_online $master
            $master set fallback [string repeat fallback 1000]
            assert_equal 1 [$master wait 1 5000]
            wait_for_condition 50 100 {
                [compression_client_io_thread [$master client list type replica]] > 0 &&
                [compression_client_io_thread [$replica client list type master]] > 0
            } else {
                fail "Uncompressed fallback connections were not assigned to IO workers"
            }
            # Rejected compression must not leave either side using compression.
            assert_equal {} [status $master total_net_repl_uncompressed_bytes]
            assert_equal {} [status $replica total_net_repl_decompressed_bytes]
            assert_equal [string repeat fallback 1000] [$replica get fallback]
        }
    }
}

# Cover matching level 1, rejected level 2, and compression without IO workers.
foreach io_threads {1 4} {
    foreach replica_compression {1 2} {
        foreach rdb_channel {no yes} {
            set master_compression 1
            set compressed [expr {$io_threads > 1 && $replica_compression == 1}]
            set overrides [list io-threads $io_threads io-threads-client-types replication-compressed \
                repl-rdb-channel $rdb_channel repl-compression $master_compression \
                repl-compression-max-latency 10 save ""]
            set context "io-threads=$io_threads compression=$replica_compression rdb-channel=$rdb_channel"

            start_server [list tags {repl iothreads external:skip} overrides $overrides] {
                start_server [list overrides [concat $overrides [list repl-compression $replica_compression]]] {
                    set master [srv -1 client]
                    set replica [srv 0 client]

                    test "Replication compression thread assignment: $context" {
                        assert_equal $io_threads [lindex [$master config get io-threads] 1]
                        foreach client [list $master $replica] {
                            assert_equal -1 [get_io_thread_clients $io_threads $client]
                            for {set tid 1} {$tid < $io_threads} {incr tid} {
                                assert_equal 0 [get_io_thread_clients $tid $client]
                            }
                            assert_equal 0 [compression_client_io_thread [$client client info]]
                        }

                        # Include data in the initial RDB as well as in the stream.
                        $master set initial [string repeat initial 1000]
                        $replica replicaof [srv -1 host] [srv -1 port]
                        wait_for_sync $replica
                        wait_replica_online $master
                        for {set i 0} {$i < 20} {incr i} {
                            $master set streamed:$i [string repeat "value:$i" 1000]
                        }
                        assert_equal 1 [$master wait 1 5000]
                        wait_for_ofs_sync $master $replica
                        assert_equal [$master debug digest] [$replica debug digest]

                        wait_for_condition 50 100 {
                            ([compression_client_io_thread [$master client list type replica]] > 0) == $compressed &&
                            ([compression_client_io_thread [$replica client list type master]] > 0) == $compressed
                        } else {
                            fail "Unexpected replication thread assignment: $context"
                        }
                        assert_equal $compressed [expr {[status $master total_net_repl_uncompressed_bytes] > 0}]
                        assert_equal $compressed [expr {[status $replica total_net_repl_decompressed_bytes] > 0}]

                        foreach client [list $master $replica] {
                            # Only the compressed replication link may use a worker.
                            for {set tid 1} {$tid < $io_threads} {incr tid} {
                                assert_equal [expr {$tid == 1 ? $compressed : 0}] [get_io_thread_clients $tid $client]
                            }
                            assert_equal 0 [compression_client_io_thread [$client client info]]
                        }
                    }

                    test "Replication compression thread assignment after partial sync: $context" {
                        set partial_syncs [status $master sync_partial_ok]
                        $replica client kill type master
                        $master set reconnected [string repeat reconnected 1000]
                        wait_for_condition 100 100 {
                            [status $master sync_partial_ok] > $partial_syncs &&
                            [$replica get reconnected] eq [string repeat reconnected 1000]
                        } else {
                            fail "Replication did not resume via partial sync: $context"
                        }
                        assert_equal 1 [$master wait 1 5000]
                        wait_for_condition 50 100 {
                            ([compression_client_io_thread [$master client list type replica]] > 0) == $compressed &&
                            ([compression_client_io_thread [$replica client list type master]] > 0) == $compressed
                        } else {
                            fail "Unexpected replication thread assignment after partial sync: $context"
                        }
                    }
                }
            }
        }
    }
}

set overrides {io-threads 4 io-threads-client-types replication-compressed repl-compression 1 repl-compression-max-latency 10 save ""}
start_server [list tags {repl iothreads external:skip} overrides $overrides] {
    start_server [list overrides $overrides] {
        start_server [list overrides $overrides] {
            start_server [list overrides [concat $overrides {io-threads 1 io-threads-client-types all repl-compression 0}]] {
                start_server [list overrides [concat $overrides {io-threads 1 io-threads-client-types all repl-compression 0}]] {
                    test {Compression-only IO threads with two compressed replicas, two uncompressed replicas and two normal clients} {
                        set master [srv -4 client]
                        set compressed_replicas [list [srv -3 client] [srv -2 client]]
                        set plain_replicas [list [srv -1 client] [srv 0 client]]
                        set replicas [concat $compressed_replicas $plain_replicas]
                        set replica_ports [list [srv -3 port] [srv -2 port] [srv -1 port] [srv 0 port]]
                        set normal_clients [list [redis_client -4] [redis_client -4]]

                        $master set initial [string repeat initial 1000]
                        foreach replica $replicas {
                            $replica replicaof [srv -4 host] [srv -4 port]
                        }
                        foreach replica $replicas {
                            wait_for_sync $replica
                        }
                        for {set i 0} {$i < 4} {incr i} {
                            wait_replica_online $master $i
                        }

                        # Both normal clients produce traffic while all four
                        # replication connections are active.
                        for {set i 0} {$i < 20} {incr i} {
                            set client [lindex $normal_clients [expr {$i % 2}]]
                            $client set streamed:$i [string repeat "value:$i" 1000]
                        }
                        foreach client $normal_clients {
                            # All four replicas must acknowledge this client's writes.
                            assert_equal 4 [$client wait 4 5000]
                        }
                        foreach replica $replicas {
                            wait_for_ofs_sync $master $replica
                            # Each replica must contain the same data as the master.
                            assert_equal [$master debug digest] [$replica debug digest]
                        }

                        # Match each connection by port, independently of
                        # connection order, and verify both ends of the link.
                        # Compressed links use workers; uncompressed links use thread 0.
                        foreach replica $replicas port $replica_ports compressed {1 1 0 0} {
                            wait_for_condition 50 100 {
                                ([compression_replica_io_thread $master $port] > 0) == $compressed &&
                                ([compression_client_io_thread [$replica client list type master]] > 0) == $compressed
                            } else {
                                fail "Unexpected thread assignment for replica on port $port: compression=$compressed"
                            }
                        }
                        # All four replicas must remain connected during these checks.
                        assert_equal 4 [status $master connected_slaves]
                        # The two compressed replica connections use separate workers.
                        assert_equal 1 [get_io_thread_clients 1 $master]
                        assert_equal 1 [get_io_thread_clients 2 $master]
                        # The remaining worker cannot accept any of the uncompressed clients.
                        assert_equal 0 [get_io_thread_clients 3 $master]
                        # The mode creates no worker beyond the configured thread count.
                        assert_equal -1 [get_io_thread_clients 4 $master]
                        foreach client $normal_clients {
                            # Both additional normal client connections stay on the main thread.
                            assert_equal 0 [compression_client_io_thread [$client client info]]
                        }
                        foreach client [concat [list $master] $replicas] {
                            # The existing test-harness connections on all five servers
                            # also stay on their respective main threads.
                            assert_equal 0 [compression_client_io_thread [$client client info]]
                        }

                        # The master actually compressed replication data; this counter
                        # measures the bytes fed into the compressor before compression.
                        assert {[status $master total_net_repl_uncompressed_bytes] > 0}
                        foreach replica $compressed_replicas {
                            # Each compressed replica's worker serves its upstream master connection.
                            assert_equal 1 [get_io_thread_clients 1 $replica]
                            # The other workers have no eligible connections.
                            assert_equal 0 [get_io_thread_clients 2 $replica]
                            assert_equal 0 [get_io_thread_clients 3 $replica]
                            # Each compressed replica actually decompressed replication data.
                            assert {[status $replica total_net_repl_decompressed_bytes] > 0}
                        }
                        foreach replica $plain_replicas {
                            # The uncompressed replicas were configured without IO workers.
                            assert_equal -1 [get_io_thread_clients 1 $replica]
                            # No bytes were decompressed; INFO omits the counter when it is zero.
                            assert_equal {} [status $replica total_net_repl_decompressed_bytes]
                        }
                        foreach client $normal_clients {
                            $client close
                        }
                    }
                }
            }
        }
    }
}
