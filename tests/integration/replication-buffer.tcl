start_server {tags {"repl external:skip"}} {
start_server {} {
start_server {} {
start_server {} {
    set slave1 [srv -3 client]
    set slave2 [srv -2 client]
    set slave3 [srv -1 client]

    set master [srv 0 client]
    set master_host [srv 0 host]
    set master_port [srv 0 port]
    # Don't bother replicas' output buffer
    $master config set repl-backlog-size 16384

    # Make sure slave3 is synchronized with master
    $slave3 slaveof $master_host $master_port
    wait_for_condition 50 100 {
        [lindex [$slave3 role] 3] eq {connected}
    } else {
        fail "fail to sync with slaves"
    }

    # Generating RDB will take some 10 seconds
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

    test {All slaves share one global replication buffer} {
        set before_used [s used_memory]
        populate 1024 "" 1024 ; # Write extra 1M data
        # New data uses 1M memory, but all replicas use only one
        # replication buffer, so all replicas output memory is not
        # more than double of replication buffer.
        set repl_buf_mem [s mem_replication_buffer]
        set extra_mem [expr {[s used_memory]-$before_used-1024*1024}]
        assert {$extra_mem < 2*$repl_buf_mem}

        # Kill slave1, replication_buffer will not become smaller
        catch {$slave1 shutdown nosave}
        wait_for_condition 50 100 {
            [s connected_slaves] eq {2}
        } else {
            fail "slave doesn't disconnect with master"
        }
        assert_equal $repl_buf_mem [s mem_replication_buffer]
    }

    test {Replication buffer will become smaller when no slave uses} {
        # Wait slave3 catch up with the master
        wait_for_condition 500 10 {
            [s master_repl_offset] eq [s -1 master_repl_offset]
        } else {
            fail "slave3's offset is not the same as master"
        }

        set repl_buf_mem [s mem_replication_buffer]
        # Kill slave2, replication_buffer will become smaller
        catch {$slave2 shutdown nosave}
        wait_for_condition 50 100 {
            [s connected_slaves] eq {1}
        } else {
            fail "slave2 doesn't disconnect with master"
        }
        assert {$repl_buf_mem > [s mem_replication_buffer]}
    }
}
}
}
}

start_server {tags {"repl external:skip"}} {
start_server {} {
start_server {} {
    set slave1 [srv -2 client]
    set slave1_pid [s -2 process_id]
    set slave2 [srv -1 client]

    set master [srv 0 client]
    set master_host [srv 0 host]
    set master_port [srv 0 port]

    $master config set save ""
    $master config set repl-backlog-size 16384
    $master config set client-output-buffer-limit "replica 0 0 0"
    $slave1 slaveof $master_host $master_port
    wait_for_sync $slave1

    test {Replication backlog size can outgrow the backlog limit config} {
         # Generating RDB will take 1000 seconds
        $master config set rdb-key-save-delay 10000000
        populate 100 master 10000
        $slave2 slaveof $master_host $master_port
        # Make sure slave2 is waiting bgsave
        wait_for_condition 5000 100 {
            ([s rdb_bgsave_in_progress] == 1) &&
            [lindex [$slave2 role] 3] eq {sync}
        } else {
            fail "fail to sync with slaves"
        }
        # Accumulate big replication backlog since slave2 kept replication buffer
        populate 10000 master 10000
        assert {[s mem_replication_backlog] > [expr 10000*10000]}
        assert {[s mem_clients_slaves] > [expr 10000*10000]}
        assert {[s mem_replication_backlog] > [s mem_clients_slaves]}
    }

    # Wait slave1 catch up with the master
    wait_for_condition 1000 100 {
        [s -2 master_repl_offset] eq [s master_repl_offset]
    } else {
        fail "Replica offset didn't catch up with the master after too long time"
    }

    test {Replica could use replication buffer (beyond backlog config) for partial re-synchronization} {
        # slave1 disconnects with master
        $slave1 slaveof [srv -1 host] [srv -1 port]
        # Write a mass of data that exceeds repl-backlog-size
        populate 10000 master 10000
        # slave1 reconnects with master
        $slave1 slaveof $master_host $master_port
        wait_for_condition 1000 100 {
            [s -2 master_repl_offset] eq [s master_repl_offset]
        } else {
            fail "Replica offset didn't catch up with the master after too long time"
        }

        # slave2 still waits for bgsave ending
        assert {[s rdb_bgsave_in_progress] eq {1} && [lindex [$slave2 role] 3] eq {sync}}
        # master accepted slave1 partial resync
        assert_equal [s sync_partial_ok] {1}
        assert_equal [$master debug digest] [$slave1 debug digest]
    }

    test {Replication backlog memory will become smaller if slaves disconnect} {
        assert {[s mem_replication_backlog] > [expr 2*10000*10000]}
        assert_equal [s connected_slaves] {2}
        catch {$slave2 shutdown nosave}
        wait_for_condition 100 10 {
            [s connected_slaves] eq {1}
        } else {
            fail "Slave2 didn't disconnect with master"
        }
        assert {[s mem_clients_slaves] < [expr 64*1024]}

        # Since we trim replication backlog inrementally, replication backlog
        # memory may take time to be reclaimed.
        wait_for_condition 1000 100 {
            [s mem_replication_backlog] < [expr 10000*10000]
        } else {
            fail "Replication backlog memory is not smaller"
        }
    }
}
}
}
