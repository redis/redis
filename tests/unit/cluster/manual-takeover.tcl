# Manual takeover test

start_cluster 5 5 {tags {external:skip cluster}} {

test "Cluster is up" {
    wait_for_cluster_state ok
}

test "Cluster is writable" {
    cluster_write_test [srv -1 port]
}

# For this test, disable replica failover until
# all of the primaries are confirmed killed. Otherwise
# there might be enough time to elect a replica.
set replica_ids { 5 6 7 }
foreach id $replica_ids {
    R $id config set cluster-replica-no-failover yes
}

set paused_pid [srv 0 pid]
set paused_pid1 [srv -1 pid]
set paused_pid2 [srv -2 pid]
test "Killing majority of master nodes" {
    pause_process $paused_pid
    pause_process $paused_pid1
    pause_process $paused_pid2
}

foreach id $replica_ids {
    R $id config set cluster-replica-no-failover no
}

test "Cluster should eventually be down" {
    for {set j 0} {$j < [llength $::servers]} {incr j} {
        if {[process_is_paused $paused_pid]} continue
        if {[process_is_paused $paused_pid1]} continue
        if {[process_is_paused $paused_pid2]} continue
        wait_for_condition 1000 50 {
            [CI $j cluster_state] eq "fail"
        } else {
            fail "Cluster node $j cluster_state:[CI $j cluster_state]"
        }
    }
}

test "Use takeover to bring slaves back" {
    foreach id $replica_ids {
        R $id cluster failover takeover
    }
}

test "Cluster should eventually be up again" {
    for {set j 0} {$j < [llength $::servers]} {incr j} {
        if {[process_is_paused $paused_pid]} continue
        if {[process_is_paused $paused_pid1]} continue
        if {[process_is_paused $paused_pid2]} continue
        wait_for_condition 1000 50 {
            [CI $j cluster_state] eq "ok"
        } else {
            fail "Cluster node $j cluster_state:[CI $j cluster_state]"
        }
    }
}

test "Cluster is writable" {
    cluster_write_test [srv -4 port]
}

test "Instance #5, #6, #7 are now masters" {
    assert {[s -5 role] eq {master}}
    assert {[s -6 role] eq {master}}
    assert {[s -7 role] eq {master}}
}

test "Restarting the previously killed master nodes" {
    resume_process $paused_pid
    resume_process $paused_pid1
    resume_process $paused_pid2
}

test "Instance #0, #1, #2 gets converted into a slaves" {
    wait_for_condition 1000 50 {
        [s 0 role] eq {slave} && [s -1 role] eq {slave} && [s -2 role] eq {slave}
    } else {
        fail "Old masters not converted into slaves"
    }
}

} ;# start_cluster
