# Replica migration test #2.
#
# Check that if 'cluster-allow-replica-migration' is set to 'no', slaves do not
# migrate when master becomes empty.

source "../tests/includes/init-tests.tcl"

# Create a cluster with 5 master and 15 slaves, to make sure there are no
# empty masters and make rebalancing simpler to handle during the test.
test "Create a 5 nodes cluster" {
    create_cluster 5 15
}

test "Cluster is up" {
    assert_cluster_state ok
}

test "Each master should have at least two replicas attached" {
    foreach_redis_id id {
        if {$id < 5} {
            wait_for_condition 1000 50 {
                [llength [lindex [R 0 role] 2]] >= 2
            } else {
                fail "Master #$id does not have 2 slaves as expected"
            }
        }
    }
}

test "Set allow-replica-migration no" {
    foreach_redis_id id {
        R $id CONFIG SET cluster-allow-replica-migration no
    }
}

set master0_id [dict get [get_myself 0] id]
test "Resharding all the master #0 slots away from it" {
    set output [exec \
        ../../../src/redis-cli --cluster rebalance \
        127.0.0.1:[get_instance_attrib redis 0 port] \
        {*}[rediscli_tls_config "../../../tests"] \
        --cluster-weight ${master0_id}=0 >@ stdout ]
}

test "Wait cluster to be stable" {
    wait_for_condition 1000 50 {
        [catch {exec ../../../src/redis-cli --cluster \
            check 127.0.0.1:[get_instance_attrib redis 0 port] \
            {*}[rediscli_tls_config "../../../tests"] \
            }] == 0
    } else {
        fail "Cluster doesn't stabilize"
    }
}

test "Master #0 stil should have its replicas" {
    assert { [llength [lindex [R 0 role] 2]] >= 2 }
}

test "Each master should have at least two replicas attached" {
    foreach_redis_id id {
        if {$id < 5} {
            wait_for_condition 1000 50 {
                [llength [lindex [R 0 role] 2]] >= 2
            } else {
                fail "Master #$id does not have 2 slaves as expected"
            }
        }
    }
}

