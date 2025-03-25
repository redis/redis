#
# Copyright (c) 2009-Present, Redis Ltd.
# All rights reserved.
#
# Copyright (c) 2024-present, Valkey contributors.
# All rights reserved.
#
# Licensed under your choice of the Redis Source Available License 2.0
# (RSALv2) or the Server Side Public License v1 (SSPLv1).
#
# Portions of this file are available under BSD3 terms; see REDISCONTRIBUTIONS for more information.
#

# Returns either main or rdbchannel client id
# Assumes there is one replica with two channels
proc get_replica_client_id {master rdbchannel} {
    set input [$master client list type replica]

    foreach line [split $input "\n"] {
        if {[regexp {id=(\d+).*flags=(\S+)} $line match id flags]} {
            if {$rdbchannel == "yes"} {
                # rdbchannel will have C flag
                if {[string match *C* $flags]} {
                    return $id
                }
            } else {
                return $id
            }
        }
    }

    error "Replica not found"
}

start_server {tags {"repl external:skip"}} {
    set replica1 [srv 0 client]

    start_server {} {
        set replica2 [srv 0 client]

        start_server {} {
            set master [srv 0 client]
            set master_host [srv 0 host]
            set master_port [srv 0 port]

            $master config set repl-diskless-sync yes
            $master config set repl-rdb-channel yes
            populate 1000 master 10

            test "Test replication with multiple replicas (rdbchannel enabled on both)" {
                $replica1 config set repl-rdb-channel yes
                $replica1 replicaof $master_host $master_port

                $replica2 config set repl-rdb-channel yes
                $replica2 replicaof $master_host $master_port

                wait_replica_online $master 0
                wait_replica_online $master 1

                $master set x 1

                # Wait until replicas catch master
                wait_for_ofs_sync $master $replica1
                wait_for_ofs_sync $master $replica2

                # Verify db's are identical
                assert_morethan [$master dbsize] 0
                assert_equal [$master get x] 1
                assert_equal [$master debug digest] [$replica1 debug digest]
                assert_equal [$master debug digest] [$replica2 debug digest]
            }

            test "Test replication with multiple replicas (rdbchannel enabled on one of them)" {
                # Allow both replicas to ask for sync
                $master config set repl-diskless-sync-delay 5

                $replica1 replicaof no one
                $replica2 replicaof no one
                $replica1 config set repl-rdb-channel yes
                $replica2 config set repl-rdb-channel no

                set loglines [count_log_lines 0]
                set prev_forks [s 0 total_forks]
                $master set x 2

                # There will be two forks subsequently, one for rdbchannel
                # replica another for the replica without rdbchannel config.
                $replica1 replicaof $master_host $master_port
                $replica2 replicaof $master_host $master_port

                # There will be two forks subsequently, one for rdbchannel
                # replica, another for the replica without rdbchannel config.
                wait_for_log_messages 0 {"*Starting BGSAVE* replicas sockets (rdb-channel)*"} $loglines 300 100
                wait_for_log_messages 0 {"*Starting BGSAVE* replicas sockets"} $loglines 300 100

                wait_replica_online $master 0 100 100
                wait_replica_online $master 1 100 100

                # Verify two new forks.
                assert_equal [s 0 total_forks] [expr $prev_forks + 2]

                wait_for_ofs_sync $master $replica1
                wait_for_ofs_sync $master $replica2

                # Verify db's are identical
                assert_equal [$replica1 get x] 2
                assert_equal [$replica2 get x] 2
                assert_equal [$master debug digest] [$replica1 debug digest]
                assert_equal [$master debug digest] [$replica2 debug digest]
            }

            test "Test rdbchannel is not used if repl-diskless-sync config is disabled on master" {
                $replica1 replicaof no one
                $replica2 replicaof no one

                $master config set repl-diskless-sync-delay 0
                $master config set repl-diskless-sync no

                $master set x 3
                $replica1 replicaof $master_host $master_port

                # Verify log message does not mention rdbchannel
                wait_for_log_messages 0 {"*Starting BGSAVE for SYNC with target: disk*"} 0 2000 1

                wait_replica_online $master 0
                wait_for_ofs_sync $master $replica1

                # Verify db's are identical
                assert_equal [$replica1 get x] 3
                assert_equal [$master debug digest] [$replica1 debug digest]
            }
        }
    }
}

start_server {tags {"repl external:skip"}} {
    set replica [srv 0 client]
    set replica_pid [srv 0 pid]

    start_server {} {
        set master [srv 0 client]
        set master_host [srv 0 host]
        set master_port [srv 0 port]

        $master config set repl-rdb-channel yes
        $replica config set repl-rdb-channel yes

        # Reuse this test to verify large key delivery
        $master config set rdbcompression no
        $master config set rdb-key-save-delay 3000
        populate 1000 prefix1 10
        populate 5 prefix2 3000000
        populate 5 prefix3 2000000
        populate 5 prefix4 1000000

        # On master info output, we should see state transition in this order:
        # 1. wait_bgsave: Replica receives psync error (+RDBCHANNELSYNC)
        # 2. send_bulk_and_stream: Replica opens rdbchannel and delivery started
        # 3. online: Sync is completed
        test "Test replica state should start with wait_bgsave" {
            $replica config set key-load-delay 100000
            # Pause replica before opening rdb channel conn
            $replica debug repl-pause before-rdb-channel
            $replica replicaof $master_host $master_port

            wait_for_condition 50 200 {
                [s 0 connected_slaves] == 1 &&
                [string match "*wait_bgsave*" [s 0 slave0]]
            } else {
                fail "replica failed"
            }
        }

        test "Test replica state advances to send_bulk_and_stream when rdbchannel connects" {
            $master set x 1
            resume_process $replica_pid

            wait_for_condition 50 200 {
                [s 0 connected_slaves] == 1 &&
                [s 0 rdb_bgsave_in_progress] == 1 &&
                [string match "*send_bulk_and_stream*" [s 0 slave0]]
            } else {
                fail "replica failed"
            }
        }

        test "Test replica rdbchannel client has SC flag on client list output" {
            set input [$master client list type replica]

            # There will two replicas, second one should be rdbchannel
            set trimmed_input [string trimright $input]
            set lines [split $trimmed_input "\n"]
            if {[llength $lines] < 2} {
                error "There is no second line in the input: $input"
            }
            set second_line [lindex $lines 1]

            # Check if 'flags=SC' exists in the second line
            if {![regexp {flags=SC} $second_line]} {
                error "Flags are not 'SC' in the second line: $second_line"
            }
        }

        test "Test replica state advances to online when fullsync is completed" {
            # Speed up loading
            $replica config set key-load-delay 0

            wait_replica_online $master 0 100 1000
            wait_for_ofs_sync $master $replica

            wait_for_condition 50 200 {
                [s 0 rdb_bgsave_in_progress] == 0 &&
                [s 0 connected_slaves] == 1 &&
                [string match "*online*" [s 0 slave0]]
            } else {
                fail "replica failed"
            }

            wait_replica_online $master 0 100 1000
            wait_for_ofs_sync $master $replica

            # Verify db's are identical
            assert_morethan [$master dbsize] 0
            assert_equal [$master debug digest] [$replica debug digest]
        }
    }
}

start_server {tags {"repl external:skip"}} {
    set replica [srv 0 client]

    start_server {} {
        set master [srv 0 client]
        set master_host [srv 0 host]
        set master_port [srv 0 port]

        $master config set repl-rdb-channel yes
        $replica config set repl-rdb-channel yes

        test "Test master memory does not increase during replication" {
            # Put some delay to rdb generation. If master doesn't forward
            # incoming traffic to replica, master's replication buffer will grow
            $master config set rdb-key-save-delay 200
            $master config set repl-backlog-size 5mb
            populate 10000 master 10000

            # Start write traffic
            set load_handle [start_write_load $master_host $master_port 100 "key1"]
            set prev_used [s 0 used_memory]

            $replica replicaof $master_host $master_port
            set backlog_size [lindex [$master config get repl-backlog-size] 1]

            # Verify used_memory stays low
            set max_retry 1000
            set prev_buf_size 0
            while {$max_retry} {
                assert_lessthan [expr [s 0 used_memory] - $prev_used] 20000000
                assert_lessthan_equal [s 0 mem_total_replication_buffers] [expr {$backlog_size + 1000000}]

                # Check replica state
                if {[string match *slave0*state=online* [$master info]] &&
                    [s -1 master_link_status] == "up"} {
                    break
                } else {
                    incr max_retry -1
                    after 10
                }
            }
            if {$max_retry == 0} {
                error "assertion:Replica not in sync after 10 seconds"
            }

            stop_write_load $load_handle
        }
    }
}

start_server {tags {"repl external:skip"}} {
    set replica [srv 0 client]

    start_server {} {
        set master [srv 0 client]
        set master_host [srv 0 host]
        set master_port [srv 0 port]

        $master config set repl-rdb-channel yes
        $replica config set repl-rdb-channel yes

        test "Test replication stream buffer becomes full on replica" {
            # For replication stream accumulation, replica inherits slave output
            # buffer limit as the size limit. In this test, we create traffic to
            # fill the buffer fully. Once the limit is reached, accumulation
            # will stop. This is not a failure scenario though. From that point,
            # further accumulation may occur on master side. Replication should
            # be completed successfully.

            # Create some artificial delay for rdb delivery and load. We'll
            # generate some traffic to fill the replication buffer.
            $master config set rdb-key-save-delay 1000
            $replica config set key-load-delay 1000
            $replica config set client-output-buffer-limit "replica 64kb 64kb 0"
            populate 2000 master 1

            set prev_sync_full [s 0 sync_full]
            $replica replicaof $master_host $master_port

            # Wait for replica to establish psync using main channel
            wait_for_condition 500 1000 {
                [string match "*state=send_bulk_and_stream*" [s 0 slave0]]
            } else {
                fail "replica didn't start sync"
            }

            # Create some traffic on replication stream
            populate 100 master 100000

            # Wait for replica's buffer limit reached
            wait_for_log_messages -1 {"*Replication buffer limit has been reached*"} 0 1000 10

            # Speed up loading
            $replica config set key-load-delay 0

            # Wait until sync is successful
            wait_for_condition 200 200 {
                [status $master master_repl_offset] eq [status $replica master_repl_offset] &&
                [status $master master_repl_offset] eq [status $replica slave_repl_offset]
            } else {
                fail "replica offsets didn't match in time"
            }

            # Verify sync was not interrupted.
            assert_equal [s 0 sync_full] [expr $prev_sync_full + 1]

            # Verify db's are identical
            assert_morethan [$master dbsize] 0
            assert_equal [$master debug digest] [$replica debug digest]
        }

        test "Test replication stream buffer config replica-full-sync-buffer-limit" {
            # By default, replica inherits client-output-buffer-limit of replica
            # to limit accumulated repl data during rdbchannel sync.
            # replica-full-sync-buffer-limit should override it if it is set.
            $replica replicaof no one

            # Create some artificial delay for rdb delivery and load. We'll
            # generate some traffic to fill the replication buffer.
            $master config set rdb-key-save-delay 1000
            $replica config set key-load-delay 1000
            $replica config set client-output-buffer-limit "replica 1024 1024 0"
            $replica config set replica-full-sync-buffer-limit 20mb
            populate 2000 master 1

            $replica replicaof $master_host $master_port

            # Wait until replication starts
            wait_for_condition 500 1000 {
                [string match "*state=send_bulk_and_stream*" [s 0 slave0]]
            } else {
                fail "replica didn't start sync"
            }

            # Create some traffic on replication stream
            populate 100 master 100000

            # Make sure config is used, we accumulated more than
            # client-output-buffer-limit
            assert_morethan [s -1 replica_full_sync_buffer_size] 1024
        }
    }
}

start_server {tags {"repl external:skip"}} {
    set master [srv 0 client]
    set master_host [srv 0 host]
    set master_port [srv 0 port]
    set master_pid  [srv 0 pid]
    set loglines [count_log_lines 0]

    $master config set repl-diskless-sync yes
    $master config set repl-rdb-channel yes
    $master config set repl-backlog-size 1mb
    $master config set client-output-buffer-limit "replica 100k 0 0"
    $master config set loglevel debug
    $master config set repl-diskless-sync-delay 3

    start_server {} {
        set replica [srv 0 client]
        set replica_pid [srv 0 pid]

        $replica config set repl-rdb-channel yes
        $replica config set loglevel debug
        $replica config set repl-timeout 10
        $replica config set key-load-delay 10000
        $replica config set loading-process-events-interval-bytes 1024

        test "Test master disconnects replica when output buffer limit is reached" {
            populate 20000 master 100 -1

            $replica replicaof $master_host $master_port
            wait_for_condition 100 200 {
                [s 0 loading] == 1
            } else {
                fail "Replica did not start loading"
            }

            # Generate some traffic for backlog ~2mb
            populate 20 master 1000000 -1

            set res [wait_for_log_messages -1 {"*Client * closed * for overcoming of output buffer limits.*"} $loglines 1000 10]
            set loglines [lindex $res 1]
            $replica config set key-load-delay 0

            # Wait until replica loads RDB
            wait_for_log_messages 0 {"*Done loading RDB*"} 0 1000 10
        }

        test "Test replication recovers after output buffer failures" {
            # Verify system is operational
            $master set x 1

            # Wait until replica catches up
            wait_replica_online $master 0 1000 100
            wait_for_ofs_sync $master $replica

            # Verify db's are identical
            assert_morethan [$master dbsize] 0
            assert_equal [$replica get x] 1
            assert_equal [$master debug digest] [$replica debug digest]
        }
    }
}

start_server {tags {"repl external:skip"}} {
    set master [srv 0 client]
    set master_host [srv 0 host]
    set master_port [srv 0 port]

    $master config set repl-diskless-sync yes
    $master config set repl-rdb-channel yes
    $master config set rdb-key-save-delay 300
    $master config set client-output-buffer-limit "replica 0 0 0"
    $master config set repl-diskless-sync-delay 5
    $master config set loglevel debug

    populate 10000 master 1

    start_server {} {
        set replica1 [srv 0 client]
        $replica1 config set repl-rdb-channel yes
        $replica1 config set loglevel debug

        start_server {} {
            set replica2 [srv 0 client]
            $replica2 config set repl-rdb-channel yes
            $replica2 config set loglevel debug

            set load_handle [start_write_load $master_host $master_port 100 "key"]

            test "Test master continues RDB delivery if not all replicas are dropped" {
                $replica1 replicaof $master_host $master_port
                $replica2 replicaof $master_host $master_port

                wait_for_condition 50 200 {
                    [s -2 rdb_bgsave_in_progress] == 1
                } else {
                    fail "Sync did not start"
                }

                # Verify replicas are connected
                wait_for_condition 500 100 {
                    [s -2 connected_slaves] == 2
                } else {
                    fail "Replicas didn't connect: [s -2 connected_slaves]"
                }

                # kill one of the replicas
                catch {$replica1 shutdown nosave}

                # Wait until replica completes full sync
                # Verify there is no other full sync attempt
                wait_for_condition 50 1000 {
                    [s 0 master_link_status] == "up" &&
                    [s -2 sync_full] == 2 &&
                    [s -2 connected_slaves] == 1
                } else {
                    fail "Sync session did not continue
                          master_link_status: [s 0 master_link_status]
                          sync_full:[s -2 sync_full]
                          connected_slaves: [s -2 connected_slaves]"
                }

                # Wait until replica catches up
                wait_replica_online $master 0 200 100
                wait_for_condition 200 100 {
                    [s 0 mem_replica_full_sync_buffer] == 0
                } else {
                    fail "Replica did not consume buffer in time"
                }
            }

            test "Test master aborts rdb delivery if all replicas are dropped" {
                $replica2 replicaof no one

                # Start replication
                $replica2 replicaof $master_host $master_port

                wait_for_condition 50 1000 {
                    [s -2 rdb_bgsave_in_progress] == 1
                } else {
                    fail "Sync did not start"
                }
                set loglines [count_log_lines -2]

                # kill replica
                catch {$replica2 shutdown nosave}

                # Verify master aborts rdb save
                wait_for_condition 50 1000 {
                    [s -2 rdb_bgsave_in_progress] == 0 &&
                    [s -2 connected_slaves] == 0
                } else {
                    fail "Master should abort the sync
                          rdb_bgsave_in_progress:[s -2 rdb_bgsave_in_progress]
                          connected_slaves: [s -2 connected_slaves]"
                }
                wait_for_log_messages -2 {"*Background transfer error*"} $loglines 1000 50
            }

            stop_write_load $load_handle
        }
    }
}

start_server {tags {"repl external:skip"}} {
    set master [srv 0 client]
    set master_host [srv 0 host]
    set master_port [srv 0 port]

    $master config set repl-diskless-sync yes
    $master config set repl-rdb-channel yes
    $master config set loglevel debug
    $master config set rdb-key-save-delay 1000

    populate 3000 prefix1 1
    populate 100 prefix2 100000

    start_server {} {
        set replica [srv 0 client]
        set replica_pid [srv 0 pid]

        $replica config set repl-rdb-channel yes
        $replica config set loglevel debug
        $replica config set repl-timeout 10

        set load_handle [start_write_load $master_host $master_port 100 "key"]

        test "Test replica recovers when rdb channel connection is killed" {
            $replica replicaof $master_host $master_port

            # Wait for sync session to start
            wait_for_condition 500 200 {
                [string match "*state=send_bulk_and_stream*" [s -1 slave0]] &&
                [s -1 rdb_bgsave_in_progress] eq 1
            } else {
                fail "replica didn't start sync session in time"
            }

            set loglines [count_log_lines -1]

            # Kill rdb channel client
            set id [get_replica_client_id $master yes]
            $master client kill id $id

            wait_for_log_messages -1 {"*Background transfer error*"} $loglines 1000 10

            # Verify master rejects main-ch-client-id after connection is killed
            assert_error {*Unrecognized*} {$master replconf main-ch-client-id $id}

            # Replica should retry
            wait_for_condition 500 200 {
                [string match "*state=send_bulk_and_stream*" [s -1 slave0]] &&
                [s -1 rdb_bgsave_in_progress] eq 1
            } else {
                fail "replica didn't retry after connection close"
            }
        }

        test "Test replica recovers when main channel connection is killed" {
            set loglines [count_log_lines -1]

            # Kill main channel client
            set id [get_replica_client_id $master yes]
            $master client kill id $id

            wait_for_log_messages -1 {"*Background transfer error*"} $loglines 1000 20

            # Replica should retry
            wait_for_condition 500 2000 {
                [string match "*state=send_bulk_and_stream*" [s -1 slave0]] &&
                [s -1 rdb_bgsave_in_progress] eq 1
            } else {
                fail "replica didn't retry after connection close"
            }
        }

        stop_write_load $load_handle

        test "Test replica recovers connection failures" {
            # Wait until replica catches up
            wait_replica_online $master 0 1000 100
            wait_for_ofs_sync $master $replica

            # Verify db's are identical
            assert_morethan [$master dbsize] 0
            assert_equal [$master debug digest] [$replica debug digest]
        }
    }
}

start_server {tags {"repl external:skip"}} {
    set replica [srv 0 client]
    set replica_pid  [srv 0 pid]

    start_server {} {
        set master [srv 0 client]
        set master_host [srv 0 host]
        set master_port [srv 0 port]

        test "Test master connection drops while streaming repl buffer into the db" {
            # Just after replica loads RDB, it will stream repl buffer into the
            # db. During streaming, we kill the master connection. Replica
            # will abort streaming and then try another psync with master.
            $master config set rdb-key-save-delay 1000
            $master config set repl-rdb-channel yes
            $master config set repl-diskless-sync yes
            $replica config set repl-rdb-channel yes
            $replica config set loading-process-events-interval-bytes 1024

            # Populate db and start write traffic
            populate 2000 master 1000
            set load_handle [start_write_load $master_host $master_port 100 "key1"]

            # Replica will pause in the loop of repl buffer streaming
            $replica debug repl-pause on-streaming-repl-buf
            $replica replicaof $master_host $master_port

            # Check if repl stream accumulation is started.
            wait_for_condition 50 1000 {
                [s -1 replica_full_sync_buffer_size] > 0
            } else {
                fail "repl stream accumulation not started"
            }

            # Wait until replica starts streaming repl buffer
            wait_for_log_messages -1 {"*Starting to stream replication buffer*"} 0 2000 10
            stop_write_load $load_handle
            $master config set rdb-key-save-delay 0

            # Kill master connection and resume the process
            $replica deferred 1
            $replica client kill type master
            $replica debug repl-pause clear
            resume_process $replica_pid
            $replica read
            $replica read
            $replica deferred 0

            wait_for_log_messages -1 {"*Master client was freed while streaming*"} 0 500 10

            # Quick check for stats test coverage
            assert_morethan_equal [s -1 replica_full_sync_buffer_peak] [s -1 replica_full_sync_buffer_size]

            # Wait until replica recovers and verify db's are identical
            wait_replica_online $master 0 1000 10
            wait_for_ofs_sync $master $replica

            assert_morethan [$master dbsize] 0
            assert_equal [$master debug digest] [$replica debug digest]
        }
    }
}

start_server {tags {"repl external:skip"}} {
    set replica [srv 0 client]
    set replica_pid  [srv 0 pid]

    start_server {} {
        set master [srv 0 client]
        set master_host [srv 0 host]
        set master_port [srv 0 port]

        test "Test main channel connection drops while loading rdb (disk based)" {
            # While loading rdb, we kill main channel connection.
            # We expect replica to complete loading RDB and then try psync
            # with the master.
            $master config set repl-rdb-channel yes
            $replica config set repl-rdb-channel yes
            $replica config set repl-diskless-load disabled
            $replica config set key-load-delay 10000
            $replica config set loading-process-events-interval-bytes 1024

            # Populate db and start write traffic
            populate 10000 master 100
            $replica replicaof $master_host $master_port

            # Wait until replica starts loading
            wait_for_condition 50 200 {
                [s -1 loading] == 1
            } else {
                fail "replica did not start loading"
            }

            # Kill replica connections
            $master client kill type replica
            $master set x 1

            # At this point, we expect replica to complete loading RDB. Then,
            # it will try psync with master.
            wait_for_log_messages -1 {"*Aborting rdb channel sync while loading the RDB*"} 0 2000 10
            wait_for_log_messages -1 {"*After loading RDB, replica will try psync with master*"} 0 2000 10

            # Speed up loading
            $replica config set key-load-delay 0

            # Wait until replica becomes online
            wait_replica_online $master 0 100 100

            # Verify there is another successful psync and no other full sync
            wait_for_condition 50 200 {
                [s 0 sync_full] == 1 &&
                [s 0 sync_partial_ok] == 1
            } else {
                fail "psync was not successful [s 0 sync_full] [s 0 sync_partial_ok]"
            }

            # Verify db's are identical after recovery
            wait_for_ofs_sync $master $replica
            assert_morethan [$master dbsize] 0
            assert_equal [$master debug digest] [$replica debug digest]
        }
    }
}

start_server {tags {"repl external:skip"}} {
    set replica [srv 0 client]
    set replica_pid  [srv 0 pid]

    start_server {} {
        set master [srv 0 client]
        set master_host [srv 0 host]
        set master_port [srv 0 port]

        test "Test main channel connection drops while loading rdb (diskless)" {
            # While loading rdb, kill both main and rdbchannel connections.
            # We expect replica to abort sync and later retry again.
            $master config set repl-rdb-channel yes
            $replica config set repl-rdb-channel yes
            $replica config set repl-diskless-load swapdb
            $replica config set key-load-delay 10000
            $replica config set loading-process-events-interval-bytes 1024

            # Populate db and start write traffic
            populate 10000 master 100

            $replica replicaof $master_host $master_port

            # Wait until replica starts loading
            wait_for_condition 50 200 {
                [s -1 loading] == 1
            } else {
                fail "replica did not start loading"
            }

            # Kill replica connections
            $master client kill type replica
            $master set x 1

            # At this point, we expect replica to abort loading RDB.
            wait_for_log_messages -1 {"*Aborting rdb channel sync while loading the RDB*"} 0 2000 10
            wait_for_log_messages -1 {"*Failed trying to load the MASTER synchronization DB from socket*"} 0 2000 10

            # Speed up loading
            $replica config set key-load-delay 0

            # Wait until replica recovers and becomes online
            wait_replica_online $master 0 100 100

            # Verify replica attempts another full sync
            wait_for_condition 50 200 {
                [s 0 sync_full] == 2 &&
                [s 0 sync_partial_ok] == 0
            } else {
                fail "sync was not successful [s 0 sync_full] [s 0 sync_partial_ok]"
            }

            # Verify db's are identical after recovery
            wait_for_ofs_sync $master $replica
            assert_morethan [$master dbsize] 0
            assert_equal [$master debug digest] [$replica debug digest]
        }
    }
}
