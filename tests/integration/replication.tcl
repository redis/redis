start_server {tags {"repl"}} {
    start_server {} {
        test {First server should have role slave after SLAVEOF} {
            r -1 slaveof [srv 0 host] [srv 0 port]
            after 1000
            s -1 role
        } {slave}

        test {BRPOPLPUSH replication, when blocking against empty list} {
            set rd [redis_deferring_client]
            $rd brpoplpush a b 5
            r lpush a foo
            wait_for_condition 50 100 {
                [r debug digest] eq [r -1 debug digest]
            } else {
                fail "Master and slave have different digest: [r debug digest] VS [r -1 debug digest]"
            }
        }

        test {BRPOPLPUSH replication, list exists} {
            set rd [redis_deferring_client]
            r lpush c 1
            r lpush c 2
            r lpush c 3
            $rd brpoplpush c d 5
            after 1000
            assert_equal [r debug digest] [r -1 debug digest]
        }
    }
}

start_server {tags {"repl"}} {
    r set mykey foo
    
    start_server {} {
        test {Second server should have role master at first} {
            s role
        } {master}
        
        test {SLAVEOF should start with link status "down"} {
            r slaveof [srv -1 host] [srv -1 port]
            s master_link_status
        } {down}
        
        test {The role should immediately be changed to "slave"} {
            s role
        } {slave}

        wait_for_sync r
        test {Sync should have transferred keys from master} {
            r get mykey
        } {foo}
        
        test {The link status should be up} {
            s master_link_status
        } {up}
        
        test {SET on the master should immediately propagate} {
            r -1 set mykey bar
            if {$::valgrind} {after 2000}
            r  0 get mykey
        } {bar}

        test {FLUSHALL should replicate} {
            r -1 flushall
            if {$::valgrind} {after 2000}
            list [r -1 dbsize] [r 0 dbsize]
        } {0 0}
    }
}

proc start_write_load {host port seconds} {
    exec tclsh8.5 tests/helpers/gen_write_load.tcl $host $port $seconds &
}

proc stop_write_load {handle} {
    catch {exec /bin/kill -9 $handle}
}

start_server {tags {"repl"}} {
    set master [srv 0 client]
    set master_host [srv 0 host]
    set master_port [srv 0 port]
    set slaves {}
    set load_handle0 [start_write_load $master_host $master_port 20]
    set load_handle1 [start_write_load $master_host $master_port 20]
    set load_handle2 [start_write_load $master_host $master_port 20]
    set load_handle3 [start_write_load $master_host $master_port 20]
    set load_handle4 [start_write_load $master_host $master_port 20]
    after 2000
    start_server {} {
        lappend slaves [srv 0 client]
        start_server {} {
            lappend slaves [srv 0 client]
            start_server {} {
                lappend slaves [srv 0 client]
                test "Connect multiple slaves at the same time (issue #141)" {
                    [lindex $slaves 0] slaveof $master_host $master_port
                    [lindex $slaves 1] slaveof $master_host $master_port
                    [lindex $slaves 2] slaveof $master_host $master_port

                    # Wait for all the three slaves to reach the "online" state
                    set retry 500
                    while {$retry} {
                        set info [r -3 info]
                        if {[string match {*slave0:*,online*slave1:*,online*slave2:*,online*} $info]} {
                            break
                        } else {
                            incr retry -1
                            after 100
                        }
                    }
                    if {$retry == 0} {
                        error "assertion:Slaves not correctly synchronized"
                    }
                    stop_write_load $load_handle0
                    stop_write_load $load_handle1
                    stop_write_load $load_handle2
                    stop_write_load $load_handle3
                    stop_write_load $load_handle4
                    set retry 10
                    while {$retry && ([$master debug digest] ne [[lindex $slaves 0] debug digest])} {
                        after 1000
                        incr retry -1
                    }
                    set digest [$master debug digest]
                    set digest0 [[lindex $slaves 0] debug digest]
                    set digest1 [[lindex $slaves 1] debug digest]
                    set digest2 [[lindex $slaves 2] debug digest]
                    assert {$digest ne 0000000000000000000000000000000000000000}
                    assert {$digest eq $digest0}
                    assert {$digest eq $digest1}
                    assert {$digest eq $digest2}
                    #puts [$master dbsize]
                    #puts [[lindex $slaves 0] dbsize]
                    #puts [[lindex $slaves 1] dbsize]
                    #puts [[lindex $slaves 2] dbsize]
                }
           }
        }
    }
}
