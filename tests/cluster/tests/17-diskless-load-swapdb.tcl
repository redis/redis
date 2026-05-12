# Check that replica keys and keys to slots map are right after failing to diskless load using SWAPDB.

source "../tests/includes/init-tests.tcl"

test "Create a primary with a replica" {
    create_cluster 1 1
}

test "Cluster should start ok" {
    assert_cluster_state ok
}

test "Cluster is writable" {
    cluster_write_test 0
}

test "Main db not affected when fail to diskless load" {
    set master [Rn 0]
    set replica [Rn 1]
    set master_id 0
    set replica_id 1

    $replica READONLY
    $replica config set repl-diskless-load swapdb
    $replica config set appendonly no
    $replica config set save ""
    $replica config rewrite
    $master config set repl-backlog-size 1024
    $master config set repl-diskless-sync yes
    $master config set repl-diskless-sync-delay 0
    $master config set rdb-key-save-delay 10000
    $master config set rdbcompression no
    $master config set appendonly no
    $master config set save ""

    # Write a key that belongs to slot 0
    set slot0_key "06S"
    $master set $slot0_key 1
    wait_for_ofs_sync $master $replica
    assert_equal {1} [$replica get $slot0_key]
    assert_equal $slot0_key [$replica CLUSTER GETKEYSINSLOT 0 1]

    # Save an RDB and kill the replica
    $replica save
    kill_instance redis $replica_id

    # Delete the key from master
    $master del $slot0_key

    # Replica must full sync with master when start because replication
    # backlog size is very small, and dumping rdb will cost several seconds.
    set num 10000
    set value [string repeat A 1024]
    set rd [redis_deferring_client redis $master_id]
    for {set j 0} {$j < $num} {incr j} {
        $rd set $j $value
    }
    for {set j 0} {$j < $num} {incr j} {
        $rd read
    }

    # Start the replica again
    restart_instance redis $replica_id
    $replica READONLY

    # Start full sync, wait till after db started loading in background
    wait_for_condition 500 10 {
        [s $replica_id async_loading] eq 1
    } else {
        fail "Fail to full sync"
    }

    # Kill master, abort full sync
    kill_instance redis $master_id

    # Start full sync, wait till the replica detects the disconnection
    wait_for_condition 500 10 {
        [s $replica_id async_loading] eq 0
    } else {
        fail "Fail to full sync"
    }

    # Replica keys and keys to slots map still both are right.
    # CLUSTERDOWN errors are acceptable here because the cluster may be in a transient state
    # due to the timing relationship with cluster-node-timeout.
    if {[catch {$replica get $slot0_key} result]} {
        assert_match "*CLUSTERDOWN*" $result
    } else {
        assert_equal {1} $result
    }

    assert_equal $slot0_key [$replica CLUSTER GETKEYSINSLOT 0 1]
}
