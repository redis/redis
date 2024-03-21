# Check that the no-failover option works

source tests/support/cluster.tcl

start_cluster 5 5 {tags {external:skip cluster}} {

test "Cluster is up" {
    wait_for_cluster_state ok
}

test "Instance #5 is a slave" {
    assert {[s -5 role] eq {slave}}

    # Configure it to never failover the master
    R 5 CONFIG SET cluster-slave-no-failover yes
}

test "Instance #5 synced with the master" {
    wait_for_condition 1000 50 {
        [s -5 master_link_status] eq {up}
    } else {
        fail "Instance #5 master link status is not up"
    }
}

test "The nofailover flag is propagated" {
    set slave5_id [dict get [cluster_get_myself 5] id]

    for {set j 0} {$j < [llength $::servers]} {incr j} {
        wait_for_condition 1000 50 {
            [cluster_has_flag [cluster_get_node_by_id $j $slave5_id] nofailover]
        } else {
            fail "Instance $id can't see the nofailover flag of slave"
        }
    }
}

test "Killing one master node" {
    pause_process [srv 0 pid]
}

test "Cluster should be still down after some time" {
    wait_for_condition 1000 50 {
        [CI 1 cluster_state] eq {fail} &&
        [CI 2 cluster_state] eq {fail} &&
        [CI 3 cluster_state] eq {fail} &&
        [CI 4 cluster_state] eq {fail} &&
        [CI 5 cluster_state] eq {fail} &&
        [CI 6 cluster_state] eq {fail} &&
        [CI 7 cluster_state] eq {fail} &&
        [CI 8 cluster_state] eq {fail} &&
        [CI 9 cluster_state] eq {fail}
    } else {
        fail "Cluster doesn't fail"
    }
}

test "Instance #5 is still a slave" {
    assert {[s -5 role] eq {slave}}
}

test "Restarting the previously killed master node" {
    resume_process [srv 0 pid]
}

} ;# start_cluster
