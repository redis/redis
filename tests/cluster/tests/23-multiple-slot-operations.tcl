# Check the multiple slot add and remove commands

source "../tests/includes/init-tests.tcl"

proc cluster_allocate_with_continuous_slots_local {n} {
    R 0 cluster ADDSLOTSRANGE 0 3276
    R 1 cluster ADDSLOTSRANGE 3277 6552
    R 2 cluster ADDSLOTSRANGE 6553 9828
    R 3 cluster ADDSLOTSRANGE 9829 13104
    R 4 cluster ADDSLOTSRANGE 13105 16383
}

proc cluster_create_with_continuous_slots_local {masters slaves} {
    cluster_allocate_with_continuous_slots_local $masters
    if {$slaves} {
        cluster_allocate_slaves $masters $slaves
    }
    assert_cluster_state ok
}


test "Create a 5 nodes cluster" {
    cluster_create_with_continuous_slots_local 5 5
}

test "Cluster should start ok" {
    assert_cluster_state ok
}

set master1 [Rn 0]
set master2 [Rn 1]
set master3 [Rn 2]
set master4 [Rn 3]
set master5 [Rn 4]

test "Continuous slots distribution" {
    assert_match "* 0-3276*" [$master1 CLUSTER NODES]
    assert_match "* 3277-6552*" [$master2 CLUSTER NODES]
    assert_match "* 6553-9828*" [$master3 CLUSTER NODES]
    assert_match "* 9829-13104*" [$master4 CLUSTER NODES]
    assert_match "* 13105-16383*" [$master5 CLUSTER NODES]
    assert_match "*0 3276*" [$master1 CLUSTER SLOTS]
    assert_match "*3277 6552*" [$master2 CLUSTER SLOTS]
    assert_match "*6553 9828*" [$master3 CLUSTER SLOTS]
    assert_match "*9829 13104*" [$master4 CLUSTER SLOTS]
    assert_match "*13105 16383*" [$master5 CLUSTER SLOTS]

    $master1 CLUSTER DELSLOTSRANGE 3001 3050
    assert_match "* 0-3000 3051-3276*" [$master1 CLUSTER NODES]
    assert_match "*0 3000*3051 3276*" [$master1 CLUSTER SLOTS]

    $master2 CLUSTER DELSLOTSRANGE 5001 5500
    assert_match "* 3277-5000 5501-6552*" [$master2 CLUSTER NODES]
    assert_match "*3277 5000*5501 6552*" [$master2 CLUSTER SLOTS]

    $master3 CLUSTER DELSLOTSRANGE 7001 7100 8001 8500
    assert_match "* 6553-7000 7101-8000 8501-9828*" [$master3 CLUSTER NODES]
    assert_match "*6553 7000*7101 8000*8501 9828*" [$master3 CLUSTER SLOTS]

    $master4 CLUSTER DELSLOTSRANGE 11001 12000 12101 12200
    assert_match "* 9829-11000 12001-12100 12201-13104*" [$master4 CLUSTER NODES]
    assert_match "*9829 11000*12001 12100*12201 13104*" [$master4 CLUSTER SLOTS]

    $master5 CLUSTER DELSLOTSRANGE 13501 14000 15001 16000
    assert_match "* 13105-13500 14001-15000 16001-16383*" [$master5 CLUSTER NODES]
    assert_match "*13105 13500*14001 15000*16001 16383*" [$master5 CLUSTER SLOTS]
}

test "ADDSLOTSRANGE command with several boundary conditions test suite" {
    # Add multiple slots with incorrect argument number
    assert_error "ERR wrong number of arguments for 'cluster|addslotsrange' command" {R 0 cluster ADDSLOTSRANGE 3001 3020 3030}

    # Add multiple slots with invalid input slot
    assert_error "ERR Invalid or out of range slot" {R 0 cluster ADDSLOTSRANGE 3001 3020 3030 aaa}
    assert_error "ERR Invalid or out of range slot" {R 0 cluster ADDSLOTSRANGE 3001 3020 3030 70000}
    assert_error "ERR Invalid or out of range slot" {R 0 cluster ADDSLOTSRANGE 3001 3020 -1000 3030}

    # Add multiple slots when start slot number is greater than the end slot
    assert_error "ERR start slot number 3030 is greater than end slot number 3025" {R 0 cluster ADDSLOTSRANGE 3001 3020 3030 3025}

    # Add multiple slots with busy slot
    assert_error "ERR Slot 3200 is already busy" {R 0 cluster ADDSLOTSRANGE 3001 3020 3200 3250}

    # Add multiple slots with assigned multiple times
    assert_error "ERR Slot 3001 specified multiple times" {R 0 cluster ADDSLOTSRANGE 3001 3020 3001 3020}
}

test "DELSLOTSRANGE command with several boundary conditions test suite" {
    # Delete multiple slots with incorrect argument number
    assert_error "ERR wrong number of arguments for 'cluster|delslotsrange' command" {R 0 cluster DELSLOTSRANGE 1000 2000 2100}
    assert_match "* 0-3000 3051-3276*" [$master1 CLUSTER NODES]
    assert_match "*0 3000*3051 3276*" [$master1 CLUSTER SLOTS]

    # Delete multiple slots with invalid input slot
    assert_error "ERR Invalid or out of range slot" {R 0 cluster DELSLOTSRANGE 1000 2000 2100 aaa}
    assert_error "ERR Invalid or out of range slot" {R 0 cluster DELSLOTSRANGE 1000 2000 2100 70000}
    assert_error "ERR Invalid or out of range slot" {R 0 cluster DELSLOTSRANGE 1000 2000 -2100 2200}
    assert_match "* 0-3000 3051-3276*" [$master1 CLUSTER NODES]
    assert_match "*0 3000*3051 3276*" [$master1 CLUSTER SLOTS]

    # Delete multiple slots when start slot number is greater than the end slot
    assert_error "ERR start slot number 5800 is greater than end slot number 5750" {R 1 cluster DELSLOTSRANGE 5600 5700 5800 5750}
    assert_match "* 3277-5000 5501-6552*" [$master2 CLUSTER NODES]
    assert_match "*3277 5000*5501 6552*" [$master2 CLUSTER SLOTS]

    # Delete multiple slots with already unassigned
    assert_error "ERR Slot 7001 is already unassigned" {R 2 cluster DELSLOTSRANGE 7001 7100 9000 9200}
    assert_match "* 6553-7000 7101-8000 8501-9828*" [$master3 CLUSTER NODES]
    assert_match "*6553 7000*7101 8000*8501 9828*" [$master3 CLUSTER SLOTS]

    # Delete multiple slots with assigned multiple times
    assert_error "ERR Slot 12500 specified multiple times" {R 3 cluster DELSLOTSRANGE 12500 12600 12500 12600}
    assert_match "* 9829-11000 12001-12100 12201-13104*" [$master4 CLUSTER NODES]
    assert_match "*9829 11000*12001 12100*12201 13104*" [$master4 CLUSTER SLOTS]
}
