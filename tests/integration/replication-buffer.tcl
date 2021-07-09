start_server {tags {"repl external:skip"}} {
    set slave1 [srv 0 client]
    start_server {} {
        set slave2 [srv 0 client]
        start_server {} {
            set slave3 [srv 0 client]
            start_server {} {
                set master [srv 0 client]
                set master_host [srv 0 host]
                set master_port [srv 0 port]

                # Make sure slave3 is synchronized with master
                $slave3 slaveof $master_host $master_port
                wait_for_condition 50 100 {
                    [lindex [$slave3 role] 3] eq {connected}
                } else {
                    fail "fail to sync with slaves"
                }

                # Generating RDB will cost several seconds
                $master config set rdb-key-save-delay 1000000
                $master config set save ""
                populate 10 "" 16

                # Make sure slave1 and slave2 are waiting bgsave
                $slave1 slaveof $master_host $master_port
                $slave2 slaveof $master_host $master_port
                wait_for_condition 50 100 {
                    ([s rdb_bgsave_in_progress] == 1) &&
                    [lindex [$slave1 role] 3] eq {sync} &&
                    [lindex [$slave2 role] 3] eq {sync}
                } else {
                    fail "fail to sync with slaves"
                }

                set before_used [s used_memory]
                populate 64 "" 1024 ; # Write extra 32K data

                test {replicas share one global replication buffer} {
                    # New data uses 32K memory, but all replicas use only one
                    # replication buffer, so all replicas output memory is not
                    # more than double of replication buffer.
                    set repl_buf_mem [s mem_replication_buffer]
                    set extra_mem [expr {[s used_memory]-$before_used-64*1024}]
                    assert {$extra_mem < 2*$repl_buf_mem}

                    # Kill slave1, replication_buffer will not become smaller
                    catch {$slave1 shutdown nosave}
                    wait_for_condition 50 100 {
                        [s slave2] eq ""
                    } else {
                        fail "slave doesn't disconnect with master"
                    }
                    assert_equal $repl_buf_mem [s mem_replication_buffer]
                }

                test {replication buffer will become smaller when no slave uses} {
                    # Wait for that slave3' offset is the same as master 
                    wait_for_condition 500 10 {
                        [s master_repl_offset] eq [lindex [$slave3 role] 4]
                    } else {
                        fail "slave's offset is not the same as master"
                    }

                    set repl_buf_mem [s mem_replication_buffer]
                    # Kill slave1, replication_buffer will become smaller
                    catch {$slave2 shutdown nosave}
                    wait_for_condition 50 100 {
                        [s slave1] eq ""
                    } else {
                        fail "slave doesn't disconnect with master"
                    }
                    assert {$repl_buf_mem > [s mem_replication_buffer]}

                    set oll 0
                    set omem 0
                    set clients [split [r client list] "\r\n"]
                    set c [lsearch -inline $clients *flags=S*]
                    if {[string length $c] > 0} {
                        assert {[regexp {oll=([0-9]+)} $c - oll]}
                        assert {[regexp {omem=([0-9]+)} $c - omem]}
                    }
                    assert_equal $oll 1
                    assert_equal $omem [s mem_replication_buffer]
                }
            }
        }
    }
}
