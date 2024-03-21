# Test UPDATE messages sent by other nodes when the currently authorirative
# master is unavailable. The test is performed in the following steps:
#
# 1) Master goes down.
# 2) Slave failover and becomes new master.
# 3) New master is partitioned away.
# 4) Old master returns.
# 5) At this point we expect the old master to turn into a slave ASAP because
#    of the UPDATE messages it will receive from the other nodes when its
#    configuration will be found to be outdated.

start_cluster 5 5 {tags {external:skip cluster}} {

test "Cluster is up" {
    wait_for_cluster_state ok
}

test "Cluster is writable" {
    cluster_write_test [srv 0 port]
}

test "Instance #5 is a slave" {
    assert {[s -5 role] eq {slave}}
}

test "Instance #5 synced with the master" {
    wait_for_condition 1000 50 {
        [s -5 master_link_status] eq {up}
    } else {
        fail "Instance #5 master link status is not up"
    }
}

set current_epoch [CI 1 cluster_current_epoch]

set paused_pid [srv 0 pid]
test "Killing one master node" {
    pause_process $paused_pid
}

test "Wait for failover" {
    wait_for_condition 1000 50 {
        [CI 1 cluster_current_epoch] > $current_epoch
    } else {
        fail "No failover detected"
    }
}

test "Cluster should eventually be up again" {
    for {set j 0} {$j < [llength $::servers]} {incr j} {
        if {[process_is_paused $paused_pid]} continue
        wait_for_condition 1000 50 {
            [CI $j cluster_state] eq "ok"
        } else {
            fail "Cluster node $j cluster_state:[CI $j cluster_state]"
        }
    }
}

test "Cluster is writable" {
    cluster_write_test [srv -1 port]
}

test "Instance #5 is now a master" {
    assert {[s -5 role] eq {master}}
}

set paused_pid5 [srv -5 pid]
test "Killing the new master #5" {
    pause_process $paused_pid5
}

test "Cluster should be down now" {
    for {set j 0} {$j < [llength $::servers]} {incr j} {
        if {[process_is_paused $paused_pid]} continue
        if {[process_is_paused $paused_pid5]} continue
        wait_for_condition 1000 50 {
            [CI $j cluster_state] eq "fail"
        } else {
            fail "Cluster node $j cluster_state:[CI $j cluster_state]"
        }
    }
}

test "Restarting the old master node" {
    resume_process $paused_pid
}

test "Instance #0 gets converted into a slave" {
    wait_for_condition 1000 50 {
        [s 0 role] eq {slave}
    } else {
        fail "Old master was not converted into slave"
    }
}

test "Restarting the new master node" {
    resume_process $paused_pid5
}

test "Cluster is up again" {
    wait_for_cluster_state ok
}

} ;# start_cluster
