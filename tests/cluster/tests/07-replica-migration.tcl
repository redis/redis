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

test "Kill slave #7 of master #2. Only slave left is #12 now" {
    kill_instance redis 7
}

set current_epoch [CI 1 cluster_current_epoch]

test "Killing master node #2, #12 should failover" {
    kill_instance redis 2
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

test "Instance 12 is now a master without slaves" {
    assert {[RI 12 role] eq {master}}
}

# The remaining instance is now without slaves. Some other slave
# should migrate to it.

test "Master #12 should get at least one migrated replica" {
    wait_for_condition 1000 50 {
        [llength [lindex [R 12 role] 2]] >= 1
    } else {
        fail "Master #12 has no replicas"
    }
}
