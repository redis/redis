start_server {tags {"aofrw"}} {

    test {Turning off AOF kills the background writing child if any} {
        r config set appendonly yes
        waitForBgrewriteaof r
        r multi
        r bgrewriteaof
        r config set appendonly no
        r exec
        set result [exec cat [srv 0 stdout] | tail -n1]
    } {*Killing*AOF*child*}

    foreach d {string int} {
        foreach e {ziplist linkedlist} {
            test "AOF rewrite of list with $e encoding, $d data" {
                r flushall
                if {$e eq {ziplist}} {set len 10} else {set len 1000}
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
        assert_match {*bgrewriteaof_scheduled:1*} [lindex $res 2]
        while {[string match {*bgrewriteaof_scheduled:1*} [r info persistence]]} {
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
        while {[string match {*bgrewriteaof_scheduled:1*} [r info persistence]]} {
            after 100
        }
    }
}
