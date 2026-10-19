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
}
