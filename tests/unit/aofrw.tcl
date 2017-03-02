start_server {tags {"aofrw"}} {
    # Enable the AOF
    r config set appendonly yes
    r config set auto-aof-rewrite-percentage 0 ; # Disable auto-rewrite.
    waitForBgrewriteaof r

    foreach rdbpre {yes no} {
        r config set aof-use-rdb-preamble $rdbpre
        test "AOF rewrite during write load: RDB preamble=$rdbpre" {
            # Start a write load for 10 seconds
            set master [srv 0 client]
            set master_host [srv 0 host]
            set master_port [srv 0 port]
            set load_handle0 [start_write_load $master_host $master_port 10]
            set load_handle1 [start_write_load $master_host $master_port 10]
            set load_handle2 [start_write_load $master_host $master_port 10]
            set load_handle3 [start_write_load $master_host $master_port 10]
            set load_handle4 [start_write_load $master_host $master_port 10]

            # Make sure the instance is really receiving data
            wait_for_condition 50 100 {
                [r dbsize] > 0
            } else {
                fail "No write load detected."
            }

            # After 3 seconds, start a rewrite, while the write load is still
            # active.
            after 3000
            r bgrewriteaof
            waitForBgrewriteaof r

            # Let it run a bit more so that we'll append some data to the new
            # AOF.
            after 1000

            # Stop the processes generating the load if they are still active
            stop_write_load $load_handle0
            stop_write_load $load_handle1
            stop_write_load $load_handle2
            stop_write_load $load_handle3
            stop_write_load $load_handle4

            # Make sure that we remain the only connected client.
            # This step is needed to make sure there are no pending writes
            # that will be processed between the two "debug digest" calls.
            wait_for_condition 50 100 {
                [llength [split [string trim [r client list]] "\n"]] == 1
            } else {
                puts [r client list]
                fail "Clients generating loads are not disconnecting"
            }

            # Get the data set digest
            set d1 [r debug digest]

            # Load the AOF
            r debug loadaof
            set d2 [r debug digest]

            # Make sure they are the same
            assert {$d1 eq $d2}
        }
    }
}

start_server {tags {"aofrw"}} {
    test {Turning off AOF kills the background writing child if any} {
        r config set appendonly yes
        waitForBgrewriteaof r
        r multi
        r bgrewriteaof
        r config set appendonly no
        r exec
        wait_for_condition 50 100 {
            [string match {*Killing*AOF*child*} [exec tail -5 < [srv 0 stdout]]]
        } else {
            fail "Can't find 'Killing AOF child' into recent logs"
        }
    }

    foreach d {string int} {
        foreach e {quicklist} {
            test "AOF rewrite of list with $e encoding, $d data" {
                r flushall
                set len 1000
                for {set j 0} {$j < $len} {incr j} {
                    if {$d eq {string}} {
                        set data [randstring 0 16 alpha]
                    } else {
                        set data [randomInt 4000000000]
                    }
                    r lpush key $data
                }
                assert_equal [r object encoding key] $e
                set d1 [r debug digest]
                r bgrewriteaof
                waitForBgrewriteaof r
                r debug loadaof
                set d2 [r debug digest]
                if {$d1 ne $d2} {
                    error "assertion:$d1 is not equal to $d2"
                }
            }
        }
    }

    foreach d {string int} {
        foreach e {intset hashtable} {
            test "AOF rewrite of set with $e encoding, $d data" {
                r flushall
                if {$e eq {intset}} {set len 10} else {set len 1000}
                for {set j 0} {$j < $len} {incr j} {
                    if {$d eq {string}} {
                        set data [randstring 0 16 alpha]
                    } else {
                        set data [randomInt 4000000000]
                    }
                    r sadd key $data
                }
                if {$d ne {string}} {
                    assert_equal [r object encoding key] $e
                }
                set d1 [r debug digest]
                r bgrewriteaof
                waitForBgrewriteaof r
                r debug loadaof
                set d2 [r debug digest]
                if {$d1 ne $d2} {
                    error "assertion:$d1 is not equal to $d2"
                }
            }
        }
    }

    foreach d {string int} {
        foreach e {ziplist hashtable} {
            test "AOF rewrite of hash with $e encoding, $d data" {
                r flushall
                if {$e eq {ziplist}} {set len 10} else {set len 1000}
                for {set j 0} {$j < $len} {incr j} {
                    if {$d eq {string}} {
                        set data [randstring 0 16 alpha]
                    } else {
                        set data [randomInt 4000000000]
                    }
                    r hset key $data $data
                }
                assert_equal [r object encoding key] $e
                set d1 [r debug digest]
                r bgrewriteaof
                waitForBgrewriteaof r
                r debug loadaof
                set d2 [r debug digest]
                if {$d1 ne $d2} {
                    error "assertion:$d1 is not equal to $d2"
                }
            }
        }
    }

    foreach d {string int} {
        foreach e {ziplist skiplist} {
            test "AOF rewrite of zset with $e encoding, $d data" {
                r flushall
                if {$e eq {ziplist}} {set len 10} else {set len 1000}
                for {set j 0} {$j < $len} {incr j} {
                    if {$d eq {string}} {
                        set data [randstring 0 16 alpha]
                    } else {
                        set data [randomInt 4000000000]
                    }
                    r zadd key [expr rand()] $data
                }
                assert_equal [r object encoding key] $e
                set d1 [r debug digest]
                r bgrewriteaof
                waitForBgrewriteaof r
                r debug loadaof
                set d2 [r debug digest]
                if {$d1 ne $d2} {
                    error "assertion:$d1 is not equal to $d2"
                }
            }
        }
    }

    test {BGREWRITEAOF is delayed if BGSAVE is in progress} {
        r multi
        r bgsave
        r bgrewriteaof
        r info persistence
        set res [r exec]
        assert_match {*scheduled*} [lindex $res 1]
        assert_match {*aof_rewrite_scheduled:1*} [lindex $res 2]
        while {[string match {*aof_rewrite_scheduled:1*} [r info persistence]]} {
            after 100
        }
    }

    test {BGREWRITEAOF is refused if already in progress} {
        catch {
            r multi
            r bgrewriteaof
            r bgrewriteaof
            r exec
        } e
        assert_match {*ERR*already*} $e
        while {[string match {*aof_rewrite_scheduled:1*} [r info persistence]]} {
            after 100
        }
    }
}
