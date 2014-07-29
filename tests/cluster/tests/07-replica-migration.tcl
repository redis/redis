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
