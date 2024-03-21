# Check the basic monitoring and failover capabilities.

start_cluster 5 5 {tags {external:skip cluster}} {

test "Cluster should start ok" {
    wait_for_cluster_state ok
}

set paused_pid5 [srv -5 pid]
set paused_pid6 [srv -6 pid]
test "Killing two slave nodes" {
    pause_process $paused_pid5
    pause_process $paused_pid6
}

test "Cluster should be still up" {
    for {set j 0} {$j < [llength $::servers]} {incr j} {
        if {[process_is_paused $paused_pid5]} continue
        if {[process_is_paused $paused_pid6]} continue
        wait_for_condition 1000 50 {
            [CI $j cluster_state] eq "ok"
        } else {
            fail "Cluster node $j cluster_state:[CI $j cluster_state]"
        }
    }
}

set paused_pid [srv -5 pid]
test "Killing one master node" {
    pause_process $paused_pid
}

# Note: the only slave of instance 0 is already down so no
# failover is possible, that would change the state back to ok.
test "Cluster should be down now" {
    for {set j 0} {$j < [llength $::servers]} {incr j} {
        if {[process_is_paused $paused_pid]} continue
        if {[process_is_paused $paused_pid5]} continue
        if {[process_is_paused $paused_pid6]} continue
        wait_for_condition 1000 50 {
            [CI $j cluster_state] eq "ok"
        } else {
            fail "Cluster node $j cluster_state:[CI $j cluster_state]"
        }
    }
}

test "Restarting master node" {
    pause_process $paused_pid
}

test "Cluster should be up again" {
    for {set j 0} {$j < [llength $::servers]} {incr j} {
        if {[process_is_paused $paused_pid5]} continue
        if {[process_is_paused $paused_pid6]} continue
        wait_for_condition 1000 50 {
            [CI $j cluster_state] eq "ok"
        } else {
            fail "Cluster node $j cluster_state:[CI $j cluster_state]"
        }
    }
}

} ;# start_cluster
