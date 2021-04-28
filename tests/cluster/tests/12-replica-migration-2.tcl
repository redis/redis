# Replica migration test #2.
#
# Check that the status of master that can be targeted by replica migration
# is acquired again, after being getting slots again, in a cluster where the
# other masters have slaves.

source "../tests/includes/init-tests.tcl"
source "../../../tests/support/cli.tcl"

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

test "Set allow-replica-migration yes" {
    foreach_redis_id id {
        R $id CONFIG SET cluster-allow-replica-migration yes
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

test "Master #0 should lose its replicas" {
    wait_for_condition 1000 50 {
        [llength [lindex [R 0 role] 2]] == 0
    } else {
        fail "Master #0 still has replicas"
    }
}

test "Resharding back some slot to master #0" {
    # Wait for the cluster config to propagate before attempting a
    # new resharding.
    after 10000
    set output [exec \
        ../../../src/redis-cli --cluster rebalance \
        127.0.0.1:[get_instance_attrib redis 0 port] \
        {*}[rediscli_tls_config "../../../tests"] \
        --cluster-weight ${master0_id}=.01 \
        --cluster-use-empty-masters  >@ stdout]
}

test "Master #0 should re-acquire one or more replicas" {
    wait_for_condition 1000 50 {
        [llength [lindex [R 0 role] 2]] >= 1
    } else {
        fail "Master #0 has no has replicas"
    }
}
