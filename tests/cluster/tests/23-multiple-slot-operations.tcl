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

test "Add multiple slots with incorrect argument number" {
    
    assert_error "ERR wrong number of arguments for 'addslotsrange' command" {R 0 cluster ADDSLOTSRANGE 3001 3020 3030}
    assert_error "ERR wrong number of arguments for 'addslotsrange' command" {R 1 cluster ADDSLOTSRANGE 5101 5120 5000}
    assert_error "ERR wrong number of arguments for 'addslotsrange' command" {R 2 cluster ADDSLOTSRANGE 7001 7100 8200}
    assert_error "ERR wrong number of arguments for 'addslotsrange' command" {R 3 cluster ADDSLOTSRANGE 11001 12000 12101}
    assert_error "ERR wrong number of arguments for 'addslotsrange' command" {R 4 cluster ADDSLOTSRANGE 13501 14000 15001}
}

test "Add multiple slots with invalid input slot" {
    
    assert_error "ERR Invalid or out of range slot" {R 0 cluster ADDSLOTSRANGE 3001 3020 3030 aaa}
    assert_error "ERR Invalid or out of range slot" {R 0 cluster ADDSLOTSRANGE 3001 3020 3030 70000}
    assert_error "ERR Invalid or out of range slot" {R 0 cluster ADDSLOTSRANGE 3001 3020 -1000 3030}
}

test "Add multiple slots when start slot number is greater than the end slot" {
    
    assert_error "ERR start slot number 3030 is greater than end slot number 3025" {R 0 cluster ADDSLOTSRANGE 3001 3020 3030 3025}
    assert_error "ERR start slot number 5500 is greater than end slot number 5400" {R 1 cluster ADDSLOTSRANGE 5001 5200 5500 5400}
    assert_error "ERR start slot number 8200 is greater than end slot number 8100" {R 2 cluster ADDSLOTSRANGE 7001 7100 8200 8100}
    assert_error "ERR start slot number 12200 is greater than end slot number 12100" {R 3 cluster ADDSLOTSRANGE 11001 12000 12200 12100}
    assert_error "ERR start slot number 15001 is greater than end slot number 14050" {R 4 cluster ADDSLOTSRANGE 13501 14000 15001 14050}
}

test "Add multiple slots with busy slot" {
    
    assert_error "ERR Slot 3200 is already busy" {R 0 cluster ADDSLOTSRANGE 3001 3020 3200 3250}
    assert_error "ERR Slot 6000 is already busy" {R 1 cluster ADDSLOTSRANGE 5001 5200 6000 6200}
    assert_error "ERR Slot 9000 is already busy" {R 2 cluster ADDSLOTSRANGE 7001 7100 9000 9200}
    assert_error "ERR Slot 13000 is already busy" {R 3 cluster ADDSLOTSRANGE 11001 12000 13000 13100}
    assert_error "ERR Slot 16100 is already busy" {R 4 cluster ADDSLOTSRANGE 13501 14000 16100 16200}
}

test "Add multiple slots with assigned multiple times" {
    
    assert_error "ERR Slot 3001 specified multiple times" {R 0 cluster ADDSLOTSRANGE 3001 3020 3001 3020}
    assert_error "ERR Slot 5001 specified multiple times" {R 1 cluster ADDSLOTSRANGE 5001 5200 5001 5200}
    assert_error "ERR Slot 7001 specified multiple times" {R 2 cluster ADDSLOTSRANGE 7001 7100 7001 7100}
    assert_error "ERR Slot 11001 specified multiple times" {R 3 cluster ADDSLOTSRANGE 11001 12000 11001 12000}
    assert_error "ERR Slot 13501 specified multiple times" {R 4 cluster ADDSLOTSRANGE 13501 14000 13501 14000}
}

test "Delete multiple slots with incorrect argument number" {
    
    assert_error "ERR wrong number of arguments for 'delslotsrange' command" {R 0 cluster DELSLOTSRANGE 1000 2000 2100}
    assert_error "ERR wrong number of arguments for 'delslotsrange' command" {R 1 cluster DELSLOTSRANGE 5600 5700 5800}
    assert_error "ERR wrong number of arguments for 'delslotsrange' command" {R 2 cluster DELSLOTSRANGE 7200 7300 7400}
    assert_error "ERR wrong number of arguments for 'delslotsrange' command" {R 3 cluster DELSLOTSRANGE 12500 12600 12700}
    assert_error "ERR wrong number of arguments for 'delslotsrange' command" {R 4 cluster DELSLOTSRANGE 14500 14600 14700}

    assert_match "* 0-3000 3051-3276*" [$master1 CLUSTER NODES]
    assert_match "*0 3000*3051 3276*" [$master1 CLUSTER SLOTS]
    assert_match "* 3277-5000 5501-6552*" [$master2 CLUSTER NODES]
    assert_match "*3277 5000*5501 6552*" [$master2 CLUSTER SLOTS]
    assert_match "* 6553-7000 7101-8000 8501-9828*" [$master3 CLUSTER NODES]
    assert_match "*6553 7000*7101 8000*8501 9828*" [$master3 CLUSTER SLOTS]
    assert_match "* 9829-11000 12001-12100 12201-13104*" [$master4 CLUSTER NODES]
    assert_match "*9829 11000*12001 12100*12201 13104*" [$master4 CLUSTER SLOTS]
    assert_match "* 13105-13500 14001-15000 16001-16383*" [$master5 CLUSTER NODES]
    assert_match "*13105 13500*14001 15000*16001 16383*" [$master5 CLUSTER SLOTS]
}

test "Delete multiple slots with invalid input slot" {
    
    assert_error "ERR Invalid or out of range slot" {R 0 cluster DELSLOTSRANGE 1000 2000 2100 aaa}
    assert_error "ERR Invalid or out of range slot" {R 0 cluster DELSLOTSRANGE 1000 2000 2100 70000}
    assert_error "ERR Invalid or out of range slot" {R 0 cluster DELSLOTSRANGE 1000 2000 -2100 2200}

    assert_match "* 0-3000 3051-3276*" [$master1 CLUSTER NODES]
    assert_match "*0 3000*3051 3276*" [$master1 CLUSTER SLOTS]
}

test "Delete multiple slots when start slot number is greater than the end slot" {
    
    assert_error "ERR start slot number 2100 is greater than end slot number 2050" {R 0 cluster DELSLOTSRANGE 1000 2000 2100 2050}
    assert_error "ERR start slot number 5800 is greater than end slot number 5750" {R 1 cluster DELSLOTSRANGE 5600 5700 5800 5750}
    assert_error "ERR start slot number 7400 is greater than end slot number 7350" {R 2 cluster DELSLOTSRANGE 7200 7300 7400 7350}
    assert_error "ERR start slot number 12700 is greater than end slot number 12650" {R 3 cluster DELSLOTSRANGE 12500 12600 12700 12650}
    assert_error "ERR start slot number 14700 is greater than end slot number 14650" {R 4 cluster DELSLOTSRANGE 14500 14600 14700 14650}
   
    assert_match "* 0-3000 3051-3276*" [$master1 CLUSTER NODES]
    assert_match "*0 3000*3051 3276*" [$master1 CLUSTER SLOTS]
    assert_match "* 3277-5000 5501-6552*" [$master2 CLUSTER NODES]
    assert_match "*3277 5000*5501 6552*" [$master2 CLUSTER SLOTS]
    assert_match "* 6553-7000 7101-8000 8501-9828*" [$master3 CLUSTER NODES]
    assert_match "*6553 7000*7101 8000*8501 9828*" [$master3 CLUSTER SLOTS]
    assert_match "* 9829-11000 12001-12100 12201-13104*" [$master4 CLUSTER NODES]
    assert_match "*9829 11000*12001 12100*12201 13104*" [$master4 CLUSTER SLOTS]
    assert_match "* 13105-13500 14001-15000 16001-16383*" [$master5 CLUSTER NODES]
    assert_match "*13105 13500*14001 15000*16001 16383*" [$master5 CLUSTER SLOTS]
}

test "Delete multiple slots with already unassigned" {
    
    assert_error "ERR Slot 3001 is already unassigned" {R 0 cluster DELSLOTSRANGE 3001 3020 3200 3250}
    assert_error "ERR Slot 5001 is already unassigned" {R 1 cluster DELSLOTSRANGE 5001 5200 6000 6200}
    assert_error "ERR Slot 7001 is already unassigned" {R 2 cluster DELSLOTSRANGE 7001 7100 9000 9200}
    assert_error "ERR Slot 11001 is already unassigned" {R 3 cluster DELSLOTSRANGE 11001 12000 13000 13100}
    assert_error "ERR Slot 13501 is already unassigned" {R 4 cluster DELSLOTSRANGE 13501 14000 16100 16200}

    assert_match "* 0-3000 3051-3276*" [$master1 CLUSTER NODES]
    assert_match "*0 3000*3051 3276*" [$master1 CLUSTER SLOTS]
    assert_match "* 3277-5000 5501-6552*" [$master2 CLUSTER NODES]
    assert_match "*3277 5000*5501 6552*" [$master2 CLUSTER SLOTS]
    assert_match "* 6553-7000 7101-8000 8501-9828*" [$master3 CLUSTER NODES]
    assert_match "*6553 7000*7101 8000*8501 9828*" [$master3 CLUSTER SLOTS]
    assert_match "* 9829-11000 12001-12100 12201-13104*" [$master4 CLUSTER NODES]
    assert_match "*9829 11000*12001 12100*12201 13104*" [$master4 CLUSTER SLOTS]
    assert_match "* 13105-13500 14001-15000 16001-16383*" [$master5 CLUSTER NODES]
    assert_match "*13105 13500*14001 15000*16001 16383*" [$master5 CLUSTER SLOTS]
}

test "Delete multiple slots with assigned multiple times" {
    
    assert_error "ERR Slot 1000 specified multiple times" {R 0 cluster DELSLOTSRANGE 1000 2000 1000 2000}
    assert_error "ERR Slot 5600 specified multiple times" {R 1 cluster DELSLOTSRANGE 5600 5700 5600 5700}
    assert_error "ERR Slot 7200 specified multiple times" {R 2 cluster DELSLOTSRANGE 7200 7300 7200 7300}
    assert_error "ERR Slot 12500 specified multiple times" {R 3 cluster DELSLOTSRANGE 12500 12600 12500 12600}
    assert_error "ERR Slot 14500 specified multiple times" {R 4 cluster DELSLOTSRANGE 14500 14600 14500 14600}

    assert_match "* 0-3000 3051-3276*" [$master1 CLUSTER NODES]
    assert_match "*0 3000*3051 3276*" [$master1 CLUSTER SLOTS]
    assert_match "* 3277-5000 5501-6552*" [$master2 CLUSTER NODES]
    assert_match "*3277 5000*5501 6552*" [$master2 CLUSTER SLOTS]
    assert_match "* 6553-7000 7101-8000 8501-9828*" [$master3 CLUSTER NODES]
    assert_match "*6553 7000*7101 8000*8501 9828*" [$master3 CLUSTER SLOTS]
    assert_match "* 9829-11000 12001-12100 12201-13104*" [$master4 CLUSTER NODES]
    assert_match "*9829 11000*12001 12100*12201 13104*" [$master4 CLUSTER SLOTS]
    assert_match "* 13105-13500 14001-15000 16001-16383*" [$master5 CLUSTER NODES]
    assert_match "*13105 13500*14001 15000*16001 16383*" [$master5 CLUSTER SLOTS]
}
