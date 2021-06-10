source "../tests/includes/init-tests.tcl"
source "../../../tests/support/cli.tcl"

test "Create a 5 nodes cluster" {
    create_cluster 5 5
}

test "Cluster should start ok" {
    assert_cluster_state ok
}

test "Cluster is writable" {
    cluster_write_test 0
}

proc find_non_empty_master {} {
    set master_id_no {}
    foreach_redis_id id {
        if {[RI $id role] eq {master} && [R $id dbsize] > 0} {
            set master_id_no $id
        }
    }
    return $master_id_no
}

proc get_one_of_my_replica {id} {
    set replica_port [lindex [lindex [lindex [R $id role] 2] 0] 1]
    set replica_id_num [get_instance_id_by_port redis $replica_port]
    return $replica_id_num
}

proc cluster_write_keys_with_expire {id ttl} {
    set prefix [randstring 20 20 alpha]
    set port [get_instance_attrib redis $id port]
    set cluster [redis_cluster 127.0.0.1:$port]
    for {set j 100} {$j < 200} {incr j} {
        $cluster setex key_expire.$j $ttl $prefix.$j
    }
    $cluster close
}

# make sure that replica who restarts from persistence will load keys
# that have already expired, critical for correct execution of commands
# that arrive from the master
proc test_slave_load_expired_keys {aof} {
    test "Slave expired keys is loaded when restarted: appendonly=$aof" {
        set master_id [find_non_empty_master]
        set replica_id [get_one_of_my_replica $master_id]

        set master_dbsize_0 [R $master_id dbsize]
        set replica_dbsize_0 [R $replica_id dbsize]
        assert_equal $master_dbsize_0 $replica_dbsize_0

        # config the replica persistency and rewrite the config file to survive restart
        # note that this needs to be done before populating the volatile keys since
        # that triggers and AOFRW, and we rather the AOF file to have 'SET PXAT' commands
        # rather than an RDB with volatile keys
        R $replica_id config set appendonly $aof
        R $replica_id config rewrite

        # fill with 100 keys with 3 second TTL
        set data_ttl 3
        cluster_write_keys_with_expire $master_id $data_ttl

        # wait for replica to be in sync with master
        wait_for_condition 500 10 {
            [R $replica_id dbsize] eq [R $master_id dbsize]
        } else {
            fail "replica didn't sync"
        }
        
        set replica_dbsize_1 [R $replica_id dbsize]
        assert {$replica_dbsize_1 > $replica_dbsize_0}

        # make replica create persistence file
        if {$aof == "yes"} {
            # we need to wait for the initial AOFRW to be done, otherwise
            # kill_instance (which now uses SIGTERM will fail ("Writing initial AOF, can't exit")
            wait_for_condition 100 10 {
                [RI $replica_id aof_rewrite_in_progress] eq 0
            } else {
                fail "keys didn't expire"
            }
        } else {
            R $replica_id save
        }

        # kill the replica (would stay down until re-started)
        kill_instance redis $replica_id

        # Make sure the master doesn't do active expire (sending DELs to the replica)
        R $master_id DEBUG SET-ACTIVE-EXPIRE 0

        # wait for all the keys to get logically expired
        after [expr $data_ttl*1000]

        # start the replica again (loading an RDB or AOF file)
        restart_instance redis $replica_id

        # make sure the keys are still there
        set replica_dbsize_3 [R $replica_id dbsize]
        assert {$replica_dbsize_3 > $replica_dbsize_0}
        
        # restore settings
        R $master_id DEBUG SET-ACTIVE-EXPIRE 1

        # wait for the master to expire all keys and replica to get the DELs
        wait_for_condition 500 10 {
            [R $replica_id dbsize] eq $master_dbsize_0
        } else {
            fail "keys didn't expire"
        }
    }
}

test_slave_load_expired_keys no
test_slave_load_expired_keys yes
