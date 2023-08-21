# This unit has the potential to create huge .reqres files, causing log-req-res-validator.py to run for a very long time...
# Since this unit doesn't do anything worth validating, reply_schema-wise, we decided to skip it
start_server {tags {"aofrw external:skip logreqres:skip"} overrides {save {}}} {
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

            # Make sure no more commands processed, before taking debug digest
            wait_load_handlers_disconnected

            # Get the data set digest
            set d1 [debug_digest]

            # Load the AOF
            r debug loadaof
            set d2 [debug_digest]

            # Make sure they are the same
            assert {$d1 eq $d2}
        }
    }
}

start_server {tags {"aofrw external:skip"} overrides {aof-use-rdb-preamble no}} {
    test {Turning off AOF kills the background writing child if any} {
        r config set appendonly yes
        waitForBgrewriteaof r

        # start a slow AOFRW
        r set k v
        r config set rdb-key-save-delay 10000000
        r bgrewriteaof

        # disable AOF and wait for the child to be killed
        r config set appendonly no
        wait_for_condition 50 100 {
            [string match {*Killing*AOF*child*} [exec tail -5 < [srv 0 stdout]]]
        } else {
            fail "Can't find 'Killing AOF child' into recent logs"
        }
        r config set rdb-key-save-delay 0
    }

    foreach d {string int} {
        foreach e {listpack quicklist} {
            test "AOF rewrite of list with $e encoding, $d data" {
                r flushall
                if {$e eq {listpack}} {
                    r config set list-max-listpack-size -2
                    set len 10
                } else {
                    r config set list-max-listpack-size 10
                    set len 1000
                }
                for {set j 0} {$j < $len} {incr j} {
                    if {$d eq {string}} {
                        set data [randstring 0 16 alpha]
                    } else {
                        set data [randomInt 4000000000]
                    }
                    r lpush key $data
                }
                assert_equal [r object encoding key] $e
                set d1 [debug_digest]
                r bgrewriteaof
                waitForBgrewriteaof r
                r debug loadaof
                set d2 [debug_digest]
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
                set d1 [debug_digest]
                r bgrewriteaof
                waitForBgrewriteaof r
                r debug loadaof
                set d2 [debug_digest]
                if {$d1 ne $d2} {
                    error "assertion:$d1 is not equal to $d2"
                }
            }
        }
    }

    foreach d {string int} {
        foreach e {listpack hashtable} {
            test "AOF rewrite of hash with $e encoding, $d data" {
                r flushall
                if {$e eq {listpack}} {set len 10} else {set len 1000}
                for {set j 0} {$j < $len} {incr j} {
                    if {$d eq {string}} {
                        set data [randstring 0 16 alpha]
                    } else {
                        set data [randomInt 4000000000]
                    }
                    r hset key $data $data
                }
                assert_equal [r object encoding key] $e
                set d1 [debug_digest]
                r bgrewriteaof
                waitForBgrewriteaof r
                r debug loadaof
                set d2 [debug_digest]
                if {$d1 ne $d2} {
                    error "assertion:$d1 is not equal to $d2"
                }
            }
        }
    }

    foreach d {string int} {
        foreach e {listpack skiplist} {
            test "AOF rewrite of zset with $e encoding, $d data" {
                r flushall
                if {$e eq {listpack}} {set len 10} else {set len 1000}
                for {set j 0} {$j < $len} {incr j} {
                    if {$d eq {string}} {
                        set data [randstring 0 16 alpha]
                    } else {
                        set data [randomInt 4000000000]
                    }
                    r zadd key [expr rand()] $data
                }
                assert_equal [r object encoding key] $e
                set d1 [debug_digest]
                r bgrewriteaof
                waitForBgrewriteaof r
                r debug loadaof
                set d2 [debug_digest]
                if {$d1 ne $d2} {
                    error "assertion:$d1 is not equal to $d2"
                }
            }
        }
    }

    test "AOF rewrite functions" {
        r flushall
        r FUNCTION LOAD {#!lua name=test
            redis.register_function('test', function() return 1 end)
        }
        r bgrewriteaof
        waitForBgrewriteaof r
        r function flush
        r debug loadaof
        assert_equal [r fcall test 0] 1
        r FUNCTION LIST
    } {{library_name test engine LUA functions {{name test description {} flags {}}}}}

    test {BGREWRITEAOF is delayed if BGSAVE is in progress} {
        r flushall
        r set k v
        r config set rdb-key-save-delay 10000000
        r bgsave
        assert_match {*scheduled*} [r bgrewriteaof]
        assert_equal [s aof_rewrite_scheduled] 1
        r config set rdb-key-save-delay 0
        catch {exec kill -9 [get_child_pid 0]}
        while {[s aof_rewrite_scheduled] eq 1} {
            after 100
        }
    }

    test {BGREWRITEAOF is refused if already in progress} {
        r config set aof-use-rdb-preamble yes
        r config set rdb-key-save-delay 10000000
        catch {
            r bgrewriteaof
            r bgrewriteaof
        } e
        assert_match {*ERR*already*} $e
        r config set rdb-key-save-delay 0
        catch {exec kill -9 [get_child_pid 0]}
    }
}
