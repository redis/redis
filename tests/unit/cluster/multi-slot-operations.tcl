# This test uses a custom slot allocation for testing
proc cluster_allocate_with_continuous_slots_local {n} {
    R 0 cluster ADDSLOTSRANGE 0 3276
    R 1 cluster ADDSLOTSRANGE 3277 6552
    R 2 cluster ADDSLOTSRANGE 6553 9828
    R 3 cluster ADDSLOTSRANGE 9829 13104
    R 4 cluster ADDSLOTSRANGE 13105 16383
}

start_cluster 5 0 {tags {external:skip cluster}} {

set master1 [srv 0 "client"]
set master2 [srv -1 "client"]
set master3 [srv -2 "client"]
set master4 [srv -3 "client"]
set master5 [srv -4 "client"]

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

test "ADDSLOTS command with several boundary conditions test suite" {
    assert_error "ERR Invalid or out of range slot" {R 0 cluster ADDSLOTS 3001 aaa}
    assert_error "ERR Invalid or out of range slot" {R 0 cluster ADDSLOTS 3001 -1000}
    assert_error "ERR Invalid or out of range slot" {R 0 cluster ADDSLOTS 3001 30003}
    
    assert_error "ERR Slot 3200 is already busy" {R 0 cluster ADDSLOTS 3200}
    assert_error "ERR Slot 8501 is already busy" {R 0 cluster ADDSLOTS 8501}

    assert_error "ERR Slot 3001 specified multiple times" {R 0 cluster ADDSLOTS 3001 3002 3001}
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
} cluster_allocate_with_continuous_slots_local

start_cluster 2 0 {tags {external:skip cluster experimental}} {

set master1 [srv 0 "client"]
set master2 [srv -1 "client"]

test "SFLUSH - Errors and output validation" {
    assert_match "* 0-8191*" [$master1 CLUSTER NODES]
    assert_match "* 8192-16383*" [$master2 CLUSTER NODES]
    assert_match "*0 8191*" [$master1 CLUSTER SLOTS]
    assert_match "*8192 16383*" [$master2 CLUSTER SLOTS]

    # make master1 non-continuous slots
    $master1 cluster DELSLOTSRANGE 1000 2000

    # Test SFLUSH errors validation
    assert_error {ERR wrong number of arguments*}           {$master1 SFLUSH 4}
    assert_error {ERR wrong number of arguments*}           {$master1 SFLUSH 4 SYNC}
    assert_error {ERR Invalid or out of range slot}         {$master1 SFLUSH x 4}
    assert_error {ERR Invalid or out of range slot}         {$master1 SFLUSH 0 12x}
    assert_error {ERR Slot 3 specified multiple times}      {$master1 SFLUSH 2 4 3 5}
    assert_error {ERR start slot number 8 is greater than*} {$master1 SFLUSH 8 4}
    assert_error {ERR wrong number of arguments*}           {$master1 SFLUSH 4 8 10}
    assert_error {ERR wrong number of arguments*}           {$master1 SFLUSH 0 999 2001 8191 ASYNCX}

    # Test SFLUSH output validation
    assert_match "" [$master1 SFLUSH 2 4]
    assert_match "" [$master1 SFLUSH 0 4]
    assert_match "" [$master2 SFLUSH 0 4]
    assert_match "" [$master1 SFLUSH 1 8191]
    assert_match "" [$master1 SFLUSH 0 8190]
    assert_match "" [$master1 SFLUSH 0 998 2001 8191]
    assert_match "" [$master1 SFLUSH 1 999 2001 8191]
    assert_match "" [$master1 SFLUSH 0 999 2001 8190]
    assert_match "" [$master1 SFLUSH 0 999 2002 8191]
    assert_match "{0 999} {2001 8191}" [$master1 SFLUSH 0 999 2001 8191]
    assert_match "{0 999} {2001 8191}" [$master1 SFLUSH 0 8191]
    assert_match "{0 999} {2001 8191}" [$master1 SFLUSH 0 4000 4001 8191]
    assert_match "" [$master2 SFLUSH 8193 16383]
    assert_match "" [$master2 SFLUSH 8192 16382]
    assert_match "{8192 16383}" [$master2 SFLUSH 8192 16383]
    assert_match "{8192 16383}" [$master2 SFLUSH 8192 16383 SYNC]
    assert_match "{8192 16383}" [$master2 SFLUSH 8192 16383 ASYNC]
    assert_match "{8192 16383}" [$master2 SFLUSH 8192 9000 9001 16383]
    assert_match "{8192 16383}" [$master2 SFLUSH 8192 9000 9001 16383 SYNC]
    assert_match "{8192 16383}" [$master2 SFLUSH 8192 9000 9001 16383 ASYNC]

    # restore master1 continuous slots
    $master1 cluster ADDSLOTSRANGE 1000 2000
}

test "SFLUSH - Deletes the keys with argument <NONE>/SYNC/ASYNC" {
    foreach op {"" "SYNC" "ASYNC"} {
        for {set i 0} {$i < 100} {incr i} {
            catch {$master1 SET key$i val$i}
            catch {$master2 SET key$i val$i}
        }

        assert {[$master1 DBSIZE] > 0}
        assert {[$master2 DBSIZE] > 0}
        if {$op eq ""} {
            assert_match "{0 8191}" [ $master1 SFLUSH 0 8191]
        } else {
            assert_match "{0 8191}" [ $master1 SFLUSH 0 8191 $op]
        }
        assert {[$master1 DBSIZE] == 0}
        assert {[$master2 DBSIZE] > 0}
        assert_match "{8192 16383}" [ $master2 SFLUSH 8192 16383]
        assert {[$master2 DBSIZE] == 0}
    }
}

}
