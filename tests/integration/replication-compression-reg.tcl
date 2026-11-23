if {$::compression} {
    set overrides [list save "" io-threads 4 repl-compression 1]
    start_server [list tags {repl external:skip} overrides $overrides] {
        start_server [list overrides $overrides] {
            set master [srv -1 client]
            set replica [srv 0 client]
            $replica replicaof [srv -1 host] [srv -1 port]
            wait_for_sync $replica
            wait_replica_online $master

            foreach data_type {compressible random} {
                test "Replication compression stays connected with 100 KiB $data_type values" {
                    set full_before [status $master sync_full]
                    set partial_before [status $master sync_partial_ok]
                    set rd [redis_deferring_client -1]
                    set value [string repeat x 102400]
                    set batches [expr {$data_type eq "compressible" ? 32 : 2}]

                    # Queue a burst of compressed input so the replica fills
                    # its input buffer while still having data to decompress.
                    # Bound the dataset by overwriting the same sixteen keys.
                    pause_process [srv 0 pid]
                    for {set batch 0} {$batch < $batches} {incr batch} {
                        for {set i 0} {$i < 16} {incr i} {
                            if {$data_type eq "random"} {
                                set value [randstring 102400 102400 binary]
                            }
                            $rd set compression-large-$i $value
                        }
                        for {set i 0} {$i < 16} {incr i} {
                            assert_equal OK [$rd read]
                        }
                    }
                    resume_process [srv 0 pid]
                    $rd wait 1 10000
                    set acknowledged [$rd read]
                    $rd close
                    assert_equal 1 $acknowledged
                    wait_for_ofs_sync $master $replica
                    assert_equal $full_before [status $master sync_full]
                    assert_equal $partial_before [status $master sync_partial_ok]
                    assert_equal [$master debug digest] [$replica debug digest]
                } {} {external:skip}
            }
        }
    }
    # The shared overrides enable compression and four IO threads on both servers.
    # Use separate RDB and command channels so writes can accumulate on the replica
    # while the snapshot is transferred. Delay each saved key to leave time for
    # the test's SET, and suppress periodic PINGs that could add unrelated bytes.
    start_server [list tags {repl external:skip} overrides [concat $overrides {
        repl-rdb-channel yes repl-diskless-sync yes
        rdb-key-save-delay 5000 repl-ping-replica-period 3600
    }]] {
        set master [srv 0 client]
        # These keys belong to the snapshot and make its save delay long enough
        # for the later SET to be sent over the command channel during full sync.
        populate 100 accounting-snapshot: 1000
        # Load the RDB directly from its connection and service network events
        # frequently, allowing the replica to buffer the separate command stream.
        start_server [list overrides [concat $overrides {
            repl-rdb-channel yes repl-diskless-load flushdb
            loading-process-events-interval-bytes 1024
        }]] {
            set replica [srv 0 client]
            test "Full sync reports decompressed replication buffer bytes" {
                $replica replicaof [srv -1 host] [srv -1 port]
                # Wait until the snapshot has started, so the SET below is sent
                # as buffered replication data rather than included in the RDB.
                wait_for_condition 500 10 {
                    [string match "*state=send_bulk_and_stream*" [status $master slave0]]
                } else {
                    fail "replication stream did not start"
                }
                # The master’s replication offset is a byte counter measured before compression,
                # so "offset after SET − offset before SET" gives the complete bytes sent.
                set before [status $master master_repl_offset]
                $master set accounting-payload [string repeat x 4096]
                set after [status $master master_repl_offset]
                set expected [expr {$after - $before}]

                # The completion message reports ctx->applied_offset. It should
                # count decoded bytes passed to the parser, not compressed bytes
                # consumed from the source buffer.
                set line [lindex [wait_for_log_messages 0 {
                    "*Successfully streamed replication buffer into the db*"
                } 0 1000 10] 0]
                # applied holds the value of `ctx.applied_offset`.
                assert [regexp {\(([0-9]+) bytes in total\)} $line -> applied]
                # the applied_offset should match the total bytes sent from the master.
                assert_equal $expected $applied
            }
        }
    }
}
