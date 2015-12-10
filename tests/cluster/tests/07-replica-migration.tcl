# Replica migration test.
# Check that orphaned masters are joined by replicas of masters having
# multiple replicas attached, according to the migration barrier settings.

source "../tests/includes/init-tests.tcl"

# Create a cluster with 5 master and 10 slaves, so that we have 2
# slaves for each master.
test "Create a 5 nodes cluster" {
    create_cluster 5 10
}

test "Cluster is up" {
    assert_cluster_state ok
}

test "Each master should have two replicas attached" {
    foreach_redis_id id {
        if {$id < 5} {
            wait_for_condition 1000 50 {
                [llength [lindex [R 0 role] 2]] == 2
            } else {
                fail "Master #$id does not have 2 slaves as expected"
            }
        }
    }
}

test "Killing all the slaves of master #0 and #1" {
    kill_instance redis 5
    kill_instance redis 10
    kill_instance redis 6
    kill_instance redis 11
    after 4000
}

foreach_redis_id id {
    if {$id < 5} {
        test "Master #$id should have at least one replica" {
            wait_for_condition 1000 50 {
                [llength [lindex [R $id role] 2]] >= 1
            } else {
                fail "Master #$id has no replicas"
            }
        }
    }
}

# Now test the migration to a master which used to be a slave, after
# a failver.

source "../tests/includes/init-tests.tcl"

# Create a cluster with 5 master and 10 slaves, so that we have 2
# slaves for each master.
test "Create a 5 nodes cluster" {
    create_cluster 5 10
}

test "Cluster is up" {
    assert_cluster_state ok
}

set failover_cycle 0
set new_master 2

while {$failover_cycle != 2} {
    incr failover_cycle
    set current_epoch [CI 1 cluster_current_epoch]

    test "Wait for slave of #$new_master to sync" {
        wait_for_condition 1000 50 {
            [string match {*state=online*} [RI $new_master slave0]]
        } else {
            fail "Slave of node #$new_master is not ok"
        }
    }

    test "Killing master node #$new_master" {
        kill_instance redis $new_master
    }

    test "Wait for failover" {
        wait_for_condition 1000 50 {
            [CI 1 cluster_current_epoch] > $current_epoch
        } else {
            fail "No failover detected"
        }
    }

    test "Cluster should eventually be up again" {
        assert_cluster_state ok
    }

    test "Cluster is writable" {
        cluster_write_test 1
    }

    test "Instance #7 or #12 is now a master" {
        assert {
            (![instance_is_killed redis 7] && [RI 7 role] eq {master}) ||
            (![instance_is_killed redis 12] && [RI 12 role] eq {master})
        }
    }

    if {![instance_is_killed redis 7] && [RI 7 role] eq {master}} {
        set new_master 7
    } else {
        set new_master 12
    }
}

# The remaining instance is now without slaves. Some other slave
# should migrate to it.

test "Master #$new_master should have at least one replica" {
    wait_for_condition 1000 50 {
        [llength [lindex [R $new_master role] 2]] >= 1
    } else {
        fail "Master #$new_master has no replicas"
    }
}
