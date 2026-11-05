# Replica migration test #2.
#
# Check that the status of master that can be targeted by replica migration
# is acquired again, after being getting slots again, in a cluster where the
# other masters have slaves.

# Create a cluster with 5 master and 15 slaves, to make sure there are no
# empty masters and make rebalancing simpler to handle during the test.
start_cluster 5 15 {tags {external:skip cluster}} {

test "Each master should have at least two replicas attached" {
    for {set id 0} {$id < 5} {incr id} {
        wait_for_condition 1000 50 {
            [llength [lindex [R $id role] 2]] >= 2
        } else {
            fail "Master #$id does not have 2 slaves as expected"
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
        fail "Master #0 has no has replicas"
    }
}

} ;# start_cluster
