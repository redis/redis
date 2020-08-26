source "../tests/includes/init-tests.tcl"

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

proc test_slave_load_expired_keys {aof} {
    test "Slave expired keys is loaded when restarted: appendonly=$aof" {
        set master_id [find_non_empty_master]
        set replica_id [get_one_of_my_replica $master_id]

        set master_dbsize [R $master_id dbsize]
        set slave_dbsize [R $replica_id dbsize]
        assert_equal $master_dbsize $slave_dbsize

        set data_ttl 5
        cluster_write_keys_with_expire $master_id $data_ttl
        after 100
        set replica_dbsize_1 [R $replica_id dbsize]
        assert {$replica_dbsize_1  > $slave_dbsize}

        R $replica_id config set appendonly $aof
        R $replica_id config rewrite

        set start_time [clock seconds]
        set end_time [expr $start_time+$data_ttl+2]
        R $replica_id save
        set replica_dbsize_2 [R $replica_id dbsize]
        assert {$replica_dbsize_2  > $slave_dbsize}
        kill_instance redis $replica_id

        set master_port [get_instance_attrib redis $master_id port]
        exec ../../../src/redis-cli -h 127.0.0.1 -p $master_port debug sleep [expr $data_ttl+3] > /dev/null &

        while {[clock seconds] <= $end_time} {
            #wait for $data_ttl seconds
        }
        restart_instance redis $replica_id

        wait_for_condition 200 50 {
            [R $replica_id ping] eq {PONG}
        } else {
            fail "replica #$replica_id not started"
        }

        set replica_dbsize_3 [R $replica_id dbsize]
        assert {$replica_dbsize_3  > $slave_dbsize}
    }
}

test_slave_load_expired_keys no
after 5000
test_slave_load_expired_keys yes
