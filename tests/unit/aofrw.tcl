start_server {tags {"aofrw"}} {
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
        foreach e {zipmap hashtable} {
            test "AOF rewrite of hash with $e encoding, $d data" {
                r flushall
                if {$e eq {zipmap}} {set len 10} else {set len 1000}
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
}
