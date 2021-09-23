# TODO top comments for these tests
start_server {tags {"repl external:skip"}} {
start_server {} {
start_server {} {
start_server {} {
    set replica1 [srv -3 client]
    set replica2 [srv -2 client]
    set replica3 [srv -1 client]

    set master [srv 0 client]
    set master_host [srv 0 host]
    set master_port [srv 0 port]
    # Make sure replication backlog not influence calculating for
    # replicas' output buffer later.
    $master config set repl-backlog-size 16384

    # Make sure replica3 is synchronized with master
    $replica3 replicaof $master_host $master_port
    wait_for_condition 50 100 {
        [lindex [$replica3 role] 3] eq {connected}
    } else {
        fail "fail to sync with replicas"
    }

    # Generating RDB will take some 100 seconds
    $master config set rdb-key-save-delay 1000000
    $master config set save ""
    populate 100 "" 16

    # Make sure replica1 and replica2 are waiting bgsave
    $replica1 replicaof $master_host $master_port
    $replica2 replicaof $master_host $master_port
    wait_for_condition 50 100 {
        ([s rdb_bgsave_in_progress] == 1) &&
        [lindex [$replica1 role] 3] eq {sync} &&
        [lindex [$replica2 role] 3] eq {sync}
    } else {
        fail "fail to sync with replicas"
    }

    test {All replicas share one global replication buffer} {
        set before_used [s used_memory]
        populate 1024 "" 1024 ; # Write extra 1M data
        # New data uses 1M memory, but all replicas use only one
        # replication buffer, so all replicas output memory is not
        # more than double of replication buffer.
        set repl_buf_mem [s mem_total_replication_buffers]
        set extra_mem [expr {[s used_memory]-$before_used-1024*1024}]
        assert {$extra_mem < 2*$repl_buf_mem}

        # Kill replica1, replication_buffer will not become smaller
        catch {$replica1 shutdown nosave}
        wait_for_condition 50 100 {
            [s connected_slaves] eq {2}
        } else {
            fail "replica doesn't disconnect with master"
        }
        assert_equal $repl_buf_mem [s mem_total_replication_buffers]
    }

    test {Replication buffer will become smaller when no replica uses} {
        # Wait replica3 catch up with the master
        wait_for_condition 500 10 {
            [s master_repl_offset] eq [s -1 master_repl_offset]
        } else {
            fail "replica3's offset is not the same as master"
        }

        set repl_buf_mem [s mem_total_replication_buffers]
        # Kill replica2, replication_buffer will become smaller
        catch {$replica2 shutdown nosave}
        wait_for_condition 50 100 {
            [s connected_slaves] eq {1}
        } else {
            fail "replica2 doesn't disconnect with master"
        }
        assert {[expr $repl_buf_mem - 1024*1024] > [s mem_total_replication_buffers]}
    }
}
}
}
}

# TODO top comments for these tests
start_server {tags {"repl external:skip"}} {
start_server {} {
start_server {} {
    set replica1 [srv -2 client]
    set replica1_pid [s -2 process_id]
    set replica2 [srv -1 client]

    set master [srv 0 client]
    set master_host [srv 0 host]
    set master_port [srv 0 port]

    $master config set save ""
    $master config set repl-backlog-size 16384
    $master config set client-output-buffer-limit "replica 0 0 0"
    $replica1 replicaof $master_host $master_port
    wait_for_sync $replica1

    test {Replication backlog size can outgrow the backlog limit config} {
        # Generating RDB will take 100 seconds
        $master config set rdb-key-save-delay 1000000
        populate 100 master 10000
        $replica2 replicaof $master_host $master_port
        # Make sure replica2 is waiting bgsave
        wait_for_condition 5000 100 {
            ([s rdb_bgsave_in_progress] == 1) &&
            [lindex [$replica2 role] 3] eq {sync}
        } else {
            fail "fail to sync with replicas"
        }
        # Accumulate big replication backlog since replica2 kept replication buffer
        populate 10000 master 10000
        assert {[s mem_replication_backlog] > [expr 10000*10000]}
        assert {[s mem_clients_slaves] > [expr 10000*10000]}
        assert {[s mem_replication_backlog] > [s mem_clients_slaves]}
    }

    # Wait replica1 catch up with the master
    wait_for_condition 1000 100 {
        [s -2 master_repl_offset] eq [s master_repl_offset]
    } else {
        fail "Replica offset didn't catch up with the master after too long time"
    }

    test {Replica could use replication buffer (beyond backlog config) for partial re-synchronization} {
        # replica1 disconnects with master
        $replica1 replicaof [srv -1 host] [srv -1 port]
        # Write a mass of data that exceeds repl-backlog-size
        populate 10000 master 10000
        # replica1 reconnects with master
        $replica1 replicaof $master_host $master_port
        wait_for_condition 1000 100 {
            [s -2 master_repl_offset] eq [s master_repl_offset]
        } else {
            fail "Replica offset didn't catch up with the master after too long time"
        }

        # replica2 still waits for bgsave ending
        assert {[s rdb_bgsave_in_progress] eq {1} && [lindex [$replica2 role] 3] eq {sync}}
        # master accepted replica1 partial resync
        assert_equal [s sync_partial_ok] {1}
        assert_equal [$master debug digest] [$replica1 debug digest]
    }

    test {Replication backlog memory will become smaller if replicas disconnect} {
        assert {[s mem_replication_backlog] > [expr 2*10000*10000]}
        assert_equal [s connected_slaves] {2}
        catch {$replica2 shutdown nosave}
        wait_for_condition 100 10 {
            [s connected_slaves] eq {1}
        } else {
            fail "replica2 didn't disconnect with master"
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
