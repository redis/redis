# Replica migration test #2.
#
# Check that a master which can be targeted by replica migration becomes a
# master again after it receives slots, in a cluster where all the other
# masters have replicas.

# Use three replicas per master so that no master is empty and rebalancing is
# easier to reason about during the test.
start_cluster 5 15 {tags {external:skip cluster valgrind:skip}} {

test "Cluster is up" {
    wait_for_cluster_state ok
}

test "Each master should have at least two replicas attached" {
    for {set id 0} {$id < 5} {incr id} {
        wait_for_condition 1000 50 {
            [llength [lindex [R $id role] 2]] >= 2
        } else {
            fail "Master #$id does not have 2 replicas as expected"
        }
    }
}

test "Set allow-replica-migration yes" {
    for {set id 0} {$id < [llength $::servers]} {incr id} {
        R $id CONFIG SET cluster-allow-replica-migration yes
    }
}

set master0_id [dict get [cluster_get_myself 0] id]
test "Resharding all the master #0 slots away from it" {
    exec src/redis-cli --cluster rebalance \
        127.0.0.1:[srv 0 port] \
        {*}[rediscli_tls_config "tests"] \
        --cluster-weight ${master0_id}=0 >@ stdout
}

test "Master #0 who lost all slots should turn into a replica without replicas" {
    wait_for_condition 2000 50 {
        [s 0 role] eq {slave} && [s 0 connected_slaves] == 0
    } else {
        puts [R 0 info replication]
        fail "Master #0 didn't turn itself into a replica"
    }
}

test "Resharding back some slot to master #0" {
    # Wait for the cluster config to propagate before attempting a
    # new resharding.
    after 10000
    exec src/redis-cli --cluster rebalance \
        127.0.0.1:[srv 0 port] \
        {*}[rediscli_tls_config "tests"] \
        --cluster-weight ${master0_id}=.01 \
        --cluster-use-empty-masters >@ stdout
}

test "Master #0 should re-acquire one or more replicas" {
    wait_for_condition 1000 50 {
        [llength [lindex [R 0 role] 2]] >= 1
    } else {
        fail "Master #0 has no replicas"
    }
}

} ;# start_cluster
