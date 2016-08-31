# Slave selection test
# Check the algorithm trying to pick the slave with the most complete history.

source "../tests/includes/init-tests.tcl"

# Create a cluster with 5 master and 10 slaves, so that we have 2
# slaves for each master.
test "Create a 5 nodes cluster" {
    create_cluster 5 10
}

test "Cluster is up" {
    assert_cluster_state ok
}

test "The first master has actually two slaves" {
    assert {[llength [lindex [R 0 role] 2]] == 2}
}

test {Slaves of #0 are instance #5 and #10 as expected} {
    set port0 [get_instance_attrib redis 0 port]
    assert {[lindex [R 5 role] 2] == $port0}
    assert {[lindex [R 10 role] 2] == $port0}
}

test "Instance #5 and #10 synced with the master" {
    wait_for_condition 1000 50 {
        [RI 5 master_link_status] eq {up} &&
        [RI 10 master_link_status] eq {up}
    } else {
        fail "Instance #5 or #10 master link status is not up"
    }
}

set cluster [redis_cluster 127.0.0.1:[get_instance_attrib redis 0 port]]

test "Slaves are both able to receive and acknowledge writes" {
    for {set j 0} {$j < 100} {incr j} {
        $cluster set $j $j
    }
    assert {[R 0 wait 2 60000] == 2}
}

test "Write data while slave #10 is paused and can't receive it" {
    # Stop the slave with a multi/exec transaction so that the master will
    # be killed as soon as it can accept writes again.
    R 10 multi
    R 10 debug sleep 10
    R 10 client kill 127.0.0.1:$port0
    R 10 deferred 1
    R 10 exec

    # Write some data the slave can't receive.
    for {set j 0} {$j < 100} {incr j} {
        $cluster set $j $j
    }

    # Prevent the master from accepting new slaves.
    # Use a large pause value since we'll kill it anyway.
    R 0 CLIENT PAUSE 60000

    # Wait for the slave to return available again
    R 10 deferred 0
    assert {[R 10 read] eq {OK OK}}

    # Kill the master so that a reconnection will not be possible.
    kill_instance redis 0
}

test "Wait for instance #5 (and not #10) to turn into a master" {
    wait_for_condition 1000 50 {
        [RI 5 role] eq {master}
    } else {
        fail "No failover detected"
    }
}

test "Wait for the node #10 to return alive before ending the test" {
    R 10 ping
}

test "Cluster should eventually be up again" {
    assert_cluster_state ok
}

test "Node #10 should eventually replicate node #5" {
    set port5 [get_instance_attrib redis 5 port]
    wait_for_condition 1000 50 {
        ([lindex [R 10 role] 2] == $port5) &&
        ([lindex [R 10 role] 3] eq {connected})
    } else {
        fail "#10 didn't became slave of #5"
    }
}
