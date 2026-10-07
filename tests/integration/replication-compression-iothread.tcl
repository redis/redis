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

start_server {tags {repl iothreads external:skip} overrides {io-threads 1 repl-compression 0} omit {repl-compression-io-thread}} {
    test {Replication compression IO thread is disabled by default and immutable} {
        assert_equal {repl-compression-io-thread no} [r config get repl-compression-io-thread]
        assert_equal -1 [get_io_thread_clients 1]
        assert_error {*immutable config*} {r config set repl-compression-io-thread yes}
    }
}

# Omit io-threads to exercise the default count. A disabled compression level
# must not create a worker, including when the build lacks compression support.
foreach level {0 1} {
    start_server [list tags {repl iothreads external:skip} \
        overrides [list repl-compression-io-thread yes repl-compression $level] omit {io-threads}] {
        test "Replication compression IO thread startup and CONFIG REWRITE: compression=$level" {
            set compression_enabled [expr {[lindex [r config get repl-compression] 1] > 0}]
            set worker_clients [expr {$compression_enabled ? 0 : -1}]
            assert_equal {io-threads 1} [r config get io-threads]
            assert_equal $compression_enabled [s io_threads_active]
            assert_equal $worker_clients [get_io_thread_clients 1]
            assert_equal -1 [get_io_thread_clients 2]
            r config rewrite
            restart_server 0 true false
            assert_equal {io-threads 1} [r config get io-threads]
            assert_equal {repl-compression-io-thread yes} [r config get repl-compression-io-thread]
            assert_equal $compression_enabled [s io_threads_active]
            assert_equal 0 [compression_client_io_thread [r client info]]
            assert_equal $worker_clients [get_io_thread_clients 1]
            assert_equal -1 [get_io_thread_clients 2]
        }
    }
}

set compression_levels {0}
if {$::compression} {
    # Matching and rejected compression requests in addition to no request.
    set compression_levels {0 1 2}
}
foreach io_threads {1 4} {
    foreach replica_compression $compression_levels {
        foreach rdb_channel {no yes} {
            set master_compression [expr {$::compression ? 1 : 0}]
            set compressed [expr {$replica_compression == 1}]
            set threaded [expr {$io_threads > 1 || $compressed}]
            set overrides [list io-threads $io_threads repl-compression-io-thread yes \
                repl-rdb-channel $rdb_channel repl-compression $master_compression \
                repl-compression-max-latency 10 save ""]
            set context "io-threads=$io_threads compression=$replica_compression rdb-channel=$rdb_channel"

            start_server [list tags {repl iothreads external:skip} overrides $overrides] {
                start_server [list overrides [concat $overrides [list repl-compression $replica_compression]]] {
                    set master [srv -1 client]
                    set replica [srv 0 client]

                    test "Replication compression thread assignment: $context" {
                        assert_equal $io_threads [lindex [$master config get io-threads] 1]
                        foreach client [list $master $replica] level [list $master_compression $replica_compression] {
                            set runtime_threads [expr {$io_threads == 1 && $level > 0 ? 2 : $io_threads}]
                            assert_equal -1 [get_io_thread_clients $runtime_threads $client]
                            assert {[get_io_thread_clients [expr {$runtime_threads - 1}] $client] >= 0}
                        }

                        foreach client [list $master $replica] {
                            set tid [compression_client_io_thread [$client client info]]
                            assert_equal [expr {$io_threads > 1}] [expr {$tid > 0}]
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
                            ([compression_client_io_thread [$master client list type replica]] > 0) == $threaded &&
                            ([compression_client_io_thread [$replica client list type master]] > 0) == $threaded
                        } else {
                            fail "Unexpected replication thread assignment: $context"
                        }
                        assert_equal $compressed [expr {[status $master total_net_repl_uncompressed_bytes] > 0}]
                        assert_equal $compressed [expr {[status $replica total_net_repl_decompressed_bytes] > 0}]

                        if {$io_threads == 1} {
                            # Only the compressed replication link may use the worker.
                            assert_equal [expr {$master_compression > 0 ? $compressed : -1}] [get_io_thread_clients 1 $master]
                            assert_equal [expr {$replica_compression > 0 ? $compressed : -1}] [get_io_thread_clients 1 $replica]
                            assert_equal 0 [compression_client_io_thread [$master client info]]
                            assert_equal 0 [compression_client_io_thread [$replica client info]]
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
                            ([compression_client_io_thread [$master client list type replica]] > 0) == $threaded &&
                            ([compression_client_io_thread [$replica client list type master]] > 0) == $threaded
                        } else {
                            fail "Unexpected replication thread assignment after partial sync: $context"
                        }
                    }
                }
            }
        }
    }
}

if {$::compression} {
    set overrides {io-threads 1 repl-compression-io-thread yes repl-compression 1 repl-compression-max-latency 10 save ""}
    start_server [list tags {repl iothreads external:skip} overrides $overrides] {
        start_server [list overrides $overrides] {
            start_server [list overrides $overrides] {
                start_server [list overrides [concat $overrides {repl-compression 0}]] {
                    start_server [list overrides [concat $overrides {repl-compression 0}]] {
                        test {Dedicated compression IO thread with two compressed replicas, two uncompressed replicas and two normal clients} {
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
                            # Compressed links use thread 1; uncompressed links use thread 0.
                            foreach replica $replicas port $replica_ports tid {1 1 0 0} {
                                wait_for_condition 50 100 {
                                    [compression_replica_io_thread $master $port] == $tid &&
                                    [compression_client_io_thread [$replica client list type master]] == $tid
                                } else {
                                    fail "Replica on port $port was not assigned to IO thread $tid"
                                }
                            }
                            # All four replicas must remain connected during these checks.
                            assert_equal 4 [status $master connected_slaves]
                            # Only the two compressed replica connections use the master's worker.
                            assert_equal 2 [get_io_thread_clients 1 $master]
                            # There is no second worker: -1 means thread 2 does not exist.
                            assert_equal -1 [get_io_thread_clients 2 $master]
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
                                # Each compressed replica actually decompressed replication data.
                                assert {[status $replica total_net_repl_decompressed_bytes] > 0}
                            }
                            foreach replica $plain_replicas {
                                # Compression is disabled, so these replicas have no IO worker.
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
}
