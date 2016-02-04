# Check the can-be-elected-as-master configuration option
# and automatic slave migration.

source "../tests/includes/init-tests.tcl"

test "Create a 3 nodes cluster" {
    create_cluster 3 6
}

test "Cluster is up" {
    assert_cluster_state ok
}

test "Cluster is writable" {
    cluster_write_test 0
}

test "Instance #3 is a slave of instance #0" {
    assert {[RI 3 master_port] eq [get_instance_attrib redis 0 port]}
}

test "Instance #6 is a slave of instance #0" {
    assert {[RI 6 master_port] eq [get_instance_attrib redis 0 port]}
}

test "Instance #3 synced with the master" {
    wait_for_condition 1000 50 {
        [RI 3 master_link_status] eq {up}
    } else {
        fail "Instance #3 master link status is not up"
    }
}

test "Instance #6 synced with the master" {
    wait_for_condition 1000 50 {
        [RI 6 master_link_status] eq {up}
    } else {
        fail "Instance #6 master link status is not up"
    }
}

test "Killing Instance #4" {
    kill_instance redis 4
}

test "Killing Instance #5" {
    kill_instance redis 5
}

proc comp.nodes.id {x y} {
    string compare [dict get $x id] [dict get $y id]
}

set slaves [lsort -command comp.nodes.id [list [get_myself 3] [get_myself 6]]]
set first_id [dict get [lindex $slaves 0] test_id]
set second_id [dict get [lindex $slaves 1] test_id]

test "Marking second instance as \"can't be master\"" {
    cluster_cant_be_elected_as_master $second_id
}

after 100

test "Killing instance #7" {
    kill_instance redis 7
}

test "First instance is not a slave of instance #1" {
    wait_for_condition 1000 60 {
        [RI $first_id master_port] eq [get_instance_attrib redis 1 port]
    } else {
        assert {1 eq 1}
    }
    assert {[RI $first_id master_port] ne [get_instance_attrib redis 1 port]}
}

test "Second instance is not a slave of instance #1" {
    assert {[RI $second_id master_port] ne [get_instance_attrib redis 1 port]}
}

test "Unmarking first instance as \"can't be master\"" {
    cluster_can_be_elected_as_master $second_id
}

test "First instance is a slave of instance #1" {
    wait_for_condition 1000 60 {
        [RI $first_id master_port] eq [get_instance_attrib redis 1 port]
    } else {
        fail "Second instance is not a slave of instance #1"
    }
}
