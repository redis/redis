# Replica migration test #3.
#
# Check that when cluster-allow-replica-migration is disabled, replicas do not
# migrate when a master becomes empty.

# Use three replicas per master so that no master is empty and rebalancing is
# easier to reason about during the test.
start_cluster 5 15 {tags {external:skip cluster}} {

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

test "Set allow-replica-migration no" {
    for {set id 0} {$id < [llength $::servers]} {incr id} {
        R $id CONFIG SET cluster-allow-replica-migration no
    }
}

set master0_id [dict get [cluster_get_myself 0] id]
test "Resharding all the master #0 slots away from it" {
    exec src/redis-cli --cluster rebalance \
        127.0.0.1:[srv 0 port] \
        {*}[rediscli_tls_config "tests"] \
        --cluster-weight ${master0_id}=0 >@ stdout
}

test "Wait cluster to be stable" {
    wait_for_cluster_stable
}

test "Master #0 still should have its replicas" {
    assert {[llength [lindex [R 0 role] 2]] >= 2}
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

} ;# start_cluster
