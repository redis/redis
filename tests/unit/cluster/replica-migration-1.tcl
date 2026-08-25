# Replica migration test.
# Check that orphaned masters are joined by replicas of masters having
# multiple replicas attached, according to the migration barrier settings.

# Create a cluster with 5 masters and 10 replicas, so that each master has 2
# replicas.
start_cluster 5 10 {tags {external:skip cluster}} {

test "Cluster is up" {
    wait_for_cluster_state ok
}

test "Each master should have two replicas attached" {
    for {set id 0} {$id < 5} {incr id} {
        wait_for_condition 1000 50 {
            [llength [lindex [R $id role] 2]] == 2
        } else {
            fail "Master #$id does not have 2 slaves as expected"
        }
    }
}

test "Killing all the slaves of master #0 and #1" {
    cluster_kill_node 5
    cluster_kill_node 10
    cluster_kill_node 6
    cluster_kill_node 11
    after 4000
}

for {set id 0} {$id < 5} {incr id} {
    test "Master #$id should have at least one replica" {
        wait_for_condition 1000 50 {
            [llength [lindex [R $id role] 2]] >= 1
        } else {
            fail "Master #$id has no replicas"
        }
    }
}

} ;# start_cluster

# Test migration to a master that used to be a replica before a failover. Use
# a fresh cluster, matching the reset performed by the legacy test.
start_cluster 5 10 {tags {external:skip cluster}} {

test "Cluster is up" {
    wait_for_cluster_state ok
}

test "Kill slave #7 of master #2. Only slave left is #12 now" {
    cluster_kill_node 7
}

set current_epoch [CI 1 cluster_current_epoch]

test "Killing master node #2, #12 should failover" {
    cluster_kill_node 2
}

test "Wait for failover" {
    wait_for_condition 1000 50 {
        [CI 1 cluster_current_epoch] > $current_epoch
    } else {
        fail "No failover detected"
    }
}

test "Cluster should eventually be up again" {
    wait_for_cluster_state ok {2 7}
}

test "Cluster is writable" {
    cluster_write_test [srv -1 port]
}

test "Instance 12 is now a master without slaves" {
    assert {[s -12 role] eq {master}}
}

# The remaining instance is now without replicas. Some other replica should
# migrate to it.
test "Master #12 should get at least one migrated replica" {
    wait_for_condition 1000 50 {
        [llength [lindex [R 12 role] 2]] >= 1
    } else {
        fail "Master #12 has no replicas"
    }
}

} ;# start_cluster
