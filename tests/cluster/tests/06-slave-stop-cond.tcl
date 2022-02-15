# Slave stop condition test
# Check that if there is a disconnection time limit, the slave will not try
# to failover its master.

source "../tests/includes/init-tests.tcl"

# Create a cluster with 5 master and 5 slaves.
test "Create a 5 nodes cluster" {
    create_cluster 5 5
}

test "Cluster is up" {
    assert_cluster_state ok
}

test "The first master has actually one slave" {
    wait_for_condition 1000 50 {
        [llength [lindex [R 0 role] 2]] == 1
    } else {
        fail "replicas didn't connect"
    }
}

test {Slaves of #0 is instance #5 as expected} {
    set port0 [get_instance_attrib redis 0 port]
    assert {[lindex [R 5 role] 2] == $port0}
}

test "Instance #5 synced with the master" {
    wait_for_condition 1000 50 {
        [RI 5 master_link_status] eq {up}
    } else {
        fail "Instance #5 master link status is not up"
    }
}

test "Lower the slave validity factor of #5 to the value of 2" {
    assert {[R 5 config set cluster-slave-validity-factor 2] eq {OK}}
}

test "Break master-slave link and prevent further reconnections" {
    # Stop the slave with a multi/exec transaction so that the master will
    # be killed as soon as it can accept writes again.
    R 5 multi
    R 5 client kill 127.0.0.1:$port0
    # here we should sleep 6 or more seconds (node_timeout * slave_validity)
    # but the actual validity time is actually incremented by the
    # repl-ping-slave-period value which is 10 seconds by default. So we
    # need to wait more than 16 seconds.
    R 5 debug sleep 20
    R 5 deferred 1
    R 5 exec

    # Prevent the master from accepting new slaves.
    # Use a large pause value since we'll kill it anyway.
    R 0 CLIENT PAUSE 60000

    # Wait for the slave to return available again
    R 5 deferred 0
    assert {[R 5 read] eq {OK OK}}

    # Kill the master so that a reconnection will not be possible.
    kill_instance redis 0
}

test "Slave #5 is reachable and alive" {
    assert {[R 5 ping] eq {PONG}}
}

test "Slave #5 should not be able to failover" {
    after 10000
    assert {[RI 5 role] eq {slave}}
}

test "Cluster should be down" {
    assert_cluster_state fail
}
