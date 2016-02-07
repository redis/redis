# Check the monitoring and failover capabilities, with a single node and
#   two empty voters.

source "../tests/includes/init-tests.tcl"

### We make sure that the test fails without the empty voters

test "Create a 1-node cluster" {
    create_cluster 1 1
}

test "Cluster should start ok" {
    assert_cluster_state ok
}

test "Cluster size is 1" {
    assert {[CI 0 cluster_size] eq 1}
}

test "Cluster is writable" {
    cluster_write_test 0
}

test "Instance #1 is a slave of instance #0" {
    assert {[RI 1 master_port] eq [get_instance_attrib redis 0 port]}
}

test "Instance #1 synced with the master" {
    wait_for_condition 1000 50 {
        [RI 1 master_link_status] eq {up}
    } else {
        fail "Instance #1 master link status is not up"
    }
}

test "Killing instance #0" {
    kill_instance redis 0
}

test "Instance #1 can't be elected for an automatic failover" {
    wait_for_condition 1000 60 {
        [RI 1 role] eq {master}
    } else {
        assert {1 eq 1}
    }
    assert {[RI 1 role] ne {master}}
}

### We make sure that the test passes with the empty voters

source "../tests/includes/init-tests.tcl"

test "Create a 1-node cluster" {
    create_cluster 1 1
}

test "Cluster should start ok" {
    assert_cluster_state ok
}

test "Cluster size is 1" {
    assert {[CI 0 cluster_size] eq 1}
}

test "Create two empty voters" {
    cluster_can_be_empty_voter 2
    cluster_can_be_empty_voter 3
}

test "Cluster size is 3" {
    wait_for_condition 1000 50 {
        [CI 1 cluster_size] eq 3
    } else {
        fail "Cluster size is not 3"
    }
}

test "Cluster is writable" {
    cluster_write_test 0
}

test "Instance #1 is a slave of instance #0" {
    assert {[RI 1 master_port] eq [get_instance_attrib redis 0 port]}
}

test "Instance #1 synced with the master" {
    wait_for_condition 1000 50 {
        [RI 1 master_link_status] eq {up}
    } else {
        fail "Instance #1 master link status is not up"
    }
}

test "Killing instance #0" {
    kill_instance redis 0
}

test "Instance #1 is elected for an automatic failover" {
    wait_for_condition 1000 60 {
        [RI 1 role] eq {master}
    } else {
        fail "Instance #1 was not elected for an automatic failover"
    }
}
