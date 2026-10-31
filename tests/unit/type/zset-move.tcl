# ZMOVE score-range batching, including memory accounting across encodings.
start_server {tags {"zset needs:debug"} overrides {key-memory-histograms yes}} {
    r debug allocsize-slots-assert 1
    foreach src_encoding {listpack btree} {
        foreach dst_encoding {listpack btree} {
            test "ZMOVE BYSCORE bounds, ties, LIMIT and destination merge - $src_encoding/$dst_encoding" {
                r del src{t} dst{t}
                r config set zset-max-listpack-entries [expr {$src_encoding eq "listpack" ? 128 : 0}]
                r zadd src{t} -inf low 0 zero 1 a 1 b 1 c 2 two 3 three +inf high
                assert_encoding $src_encoding src{t}
                r config set zset-max-listpack-entries [expr {$dst_encoding eq "listpack" ? 128 : 0}]
                r zadd dst{t} 99 a 100 retained
                assert_encoding $dst_encoding dst{t}
                r pexpire src{t} 60000
                r pexpire dst{t} 60000
                set src_expiry [r pexpiretime src{t}]
                set dst_expiry [r pexpiretime dst{t}]

                # The limit cuts through score ties, ordered lexicographically.
                assert_equal {{a 1} {b 1}} [r zmove src{t} dst{t} byscore (0 2 limit 2]
                assert_equal {low -inf zero 0 c 1 two 2 three 3 high inf} [r zrange src{t} 0 -1 withscores]
                assert_equal {a 1 b 1 retained 100} [r zrange dst{t} 0 -1 withscores]
                assert_equal $src_expiry [r pexpiretime src{t}]
                assert_equal $dst_expiry [r pexpiretime dst{t}]
                assert_equal {{c 1}} [r zmove src{t} dst{t} BYSCORE 1 (2]
                assert_equal {} [r zmove src{t} dst{t} BYSCORE 2 3 LIMIT 0]
                assert_equal {{two 2} {three 3}} [r zmove src{t} dst{t} BYSCORE 2 3 LIMIT 2147483647]
                assert_equal {{low -inf}} [r zmove src{t} dst{t} BYSCORE -inf +inf LIMIT 1]
                assert_equal {{zero 0} {high inf}} [r zmove src{t} dst{t} BYSCORE -inf +inf]
                assert_equal 0 [r exists src{t}]
                assert_equal $dst_expiry [r pexpiretime dst{t}]
            }
        }
    }

    test {ZMOVE BYSCORE empty ranges, missing keys and same-key moves have no effects} {
        r del src{t} dst{t} missing{t}
        r zadd src{t} 1 a 2 b
        set before [r dump src{t}]
        foreach bounds {{3 2} {(1 (2} {9 +inf}} {
            lassign $bounds min max
            assert_equal {} [r zmove src{t} dst{t} BYSCORE $min $max]
        }
        assert_equal {} [r zmove src{t} dst{t} BYSCORE -inf +inf LIMIT 0]
        assert_equal {} [r zmove src{t} src{t} BYSCORE -inf +inf]
        assert_equal {} [r zmove missing{t} dst{t} BYSCORE -inf +inf]
        assert_equal $before [r dump src{t}]
        assert_equal 0 [r exists dst{t}]
    }

    test {ZMOVE BYSCORE rejects invalid syntax and wrong types before mutation} {
        r del src{t} dst{t} wrong{t}
        r zadd src{t} 1 a 2 b
        r set wrong{t} string
        set before [r dump src{t}]
        foreach args {
            {BYSCORE} {BYSCORE 1} {BYSCORE 1 2 LIMIT}
            {BYSCORE 1 2 LIMIT 1 extra} {MIN extra} {MAX LIMIT 1}
        } {
            assert_error {*wrong number of arguments*} {r zmove src{t} dst{t} {*}$args}
        }
        foreach args {
            {BYSCORE nan 2} {BYSCORE 1 nan} {BYSCORE invalid 2} {BYSCORE 1 (invalid}
        } {
            assert_error {*min or max is not a float*} {r zmove src{t} dst{t} {*}$args}
        }
        foreach count {-1 1.5 bogus 9223372036854775808} {
            assert_error {*LIMIT must be a non-negative integer*} {r zmove src{t} dst{t} BYSCORE 1 2 LIMIT $count}
        }
        assert_error {*syntax error*} {r zmove src{t} dst{t} BYSCORE 1 2 COUNT 1}
        assert_error {*WRONGTYPE*} {r zmove wrong{t} dst{t} BYSCORE 1 2}
        assert_error {*WRONGTYPE*} {r zmove src{t} wrong{t} BYSCORE 1 2}
        assert_equal $before [r dump src{t}]
        assert_equal string [r get wrong{t}]
        assert_equal 0 [r exists dst{t}]
    }

    foreach resp {2 3} {
        test "ZMOVE BYSCORE returns nested pairs including integer and binary members in RESP$resp" {
            r hello $resp
            r del src{t} dst{t}
            r config set zset-max-listpack-entries 128
            set binary "a\x00b"
            r zadd src{t} 1 42 2 $binary
            assert_encoding listpack src{t}
            r readraw 1
            assert_equal {*2} [r zmove src{t} dst{t} BYSCORE -inf +inf]
            assert_equal {*2} [r read]
            assert_equal {$2} [r read]
            assert_equal 42 [r read]
            if {$resp == 2} {
                assert_equal {$1} [r read]
                assert_equal 1 [r read]
            } else {
                assert_equal ,1 [r read]
            }
            assert_equal {*2} [r read]
            assert_equal {$3} [r read]
            assert_equal $binary [r read]
            if {$resp == 2} {
                assert_equal {$1} [r read]
                assert_equal 2 [r read]
            } else {
                assert_equal ,2 [r read]
            }
            assert_equal {*0} [r zmove src{t} dst{t} BYSCORE -inf +inf]
            r readraw 0
            r hello 2
            assert_equal [list 42 1 $binary 2] [r zrange dst{t} 0 -1 withscores]
        }
    }

    test {ZMOVE BYSCORE drains B+tree batches across leaf boundaries and rehashes} {
        r del src{t} dst{t}
        r config set zset-max-listpack-entries 0
        set args {}
        for {set i 0} {$i < 1000} {incr i} {
            lappend args [expr {$i / 5}] [format m%04d $i]
        }
        r zadd src{t} {*}$args
        set expected [r zrange src{t} 0 -1 withscores]
        set moved {}
        while {[r zcard src{t}] > 0} {
            set batch [r zmove src{t} dst{t} BYSCORE -inf +inf LIMIT 17]
            assert {[llength $batch] > 0 && [llength $batch] <= 17}
            foreach pair $batch { lappend moved {*}$pair }
        }
        assert_equal $expected $moved
        assert_equal $expected [r zrange dst{t} 0 -1 withscores]
        assert_equal 0 [r exists src{t}]
    }

    test {ZMOVE BYSCORE invalidates WATCH only for modified logical keys} {
        r del src{t} dst{t}
        r zadd src{t} 1 a
        r zadd dst{t} 1 a
        set watcher [redis_client]
        $watcher watch src{t}
        r zmove src{t} dst{t} BYSCORE -inf +inf
        $watcher multi
        $watcher ping
        assert_equal {} [$watcher exec]

        r zadd src{t} 1 a
        $watcher watch dst{t}
        r zmove src{t} dst{t} BYSCORE -inf +inf
        $watcher multi
        $watcher ping
        assert_equal PONG [$watcher exec]
        $watcher close
    }

    test {ZMOVE BYSCORE propagates only successful batches with their range and limit} {
        r del src{t} dst{t}
        set repl [attach_to_replication_stream]
        r zadd src{t} 1 a 1 b 2 c
        r zmove src{t} dst{t} BYSCORE 1 2 LIMIT 2
        r zmove src{t} dst{t} BYSCORE 9 +inf
        r zmove src{t} dst{t} BYSCORE -inf +inf LIMIT 0
        r zmove src{t} dst{t} BYSCORE -inf +inf
        r zmove src{t} dst{t} BYSCORE -inf +inf
        r set marker{t} done
        assert_replication_stream $repl {
            {select *}
            {zadd src{t} 1 a 1 b 2 c}
            {zmove src{t} dst{t} BYSCORE 1 2 LIMIT 2}
            {zmove src{t} dst{t} BYSCORE -inf +inf}
            {set marker{t} done}
        }
        close_replication_stream $repl
    } {} {needs:repl}

    test {ZMOVE BYSCORE updates KEYSIZES for additions, collisions and source deletion} {
        r flushdb
        r zadd src{t} 1 a 1 b 1 c 1 d
        r zadd dst{t} 99 a 99 b
        r zmove src{t} dst{t} BYSCORE 1 1 LIMIT 2
        assert_match {*db*_distrib_zsets_items:2=2*} [r info keysizes]
        r zmove src{t} dst{t} BYSCORE -inf +inf
        assert_match {*db*_distrib_zsets_items:4=1*} [r info keysizes]
    }

    test {ZMOVE BYSCORE preserves writer updates after a batch is claimed} {
        r del src{t} dst{t}
        r zadd src{t} 100 bookmark
        assert_equal {{bookmark 100}} [r zmove src{t} dst{t} BYSCORE -inf (101 LIMIT 10]
        r zadd src{t} 200 bookmark
        r del dst{t}
        assert_equal 200 [r zscore src{t} bookmark]
    }

    test {ZMOVE BYSCORE handles destination conversion without invalidating the source iterator} {
        r del src{t} dst{t}
        r config set zset-max-listpack-entries 4
        r zadd src{t} 1 a 2 b 3 c 4 d
        r zadd dst{t} 5 e 6 f
        assert_encoding listpack src{t}
        assert_encoding listpack dst{t}
        assert_equal {{a 1} {b 2} {c 3} {d 4}} [r zmove src{t} dst{t} BYSCORE -inf +inf]
        assert_encoding btree dst{t}
        assert_equal {a 1 b 2 c 3 d 4 e 5 f 6} [r zrange dst{t} 0 -1 withscores]
    }

    test {ZMOVE BYSCORE wakes a blocked consumer of its destination} {
        r del src{t} dst{t}
        set rd [redis_deferring_client]
        $rd bzpopmin dst{t} 0
        wait_for_blocked_client
        r zadd src{t} 1 a 2 b
        assert_equal {{a 1} {b 2}} [r zmove src{t} dst{t} BYSCORE -inf +inf]
        assert_equal {dst{t} a 1} [$rd read]
        assert_equal {b 2} [r zrange dst{t} 0 -1 withscores]
        $rd close
    }

    test {ZMOVE BYSCORE emits source removal, deletion and destination addition events} {
        r del src{t} dst{t}
        r zadd src{t} 1 a 2 b
        r config set notify-keyspace-events Kzg
        set rd [redis_deferring_client]
        assert_equal 1 [psubscribe $rd __keyspace@*]
        r zmove src{t} dst{t} BYSCORE -inf +inf
        foreach {key event} {src{t} zrem src{t} del dst{t} zadd} {
            set msg [$rd read]
            assert_match "__keyspace@*:$key" [lindex $msg 2]
            assert_equal $event [lindex $msg 3]
        }
        $rd close
        r config set notify-keyspace-events {}
    }
}

start_server {tags {"zset needs:debug external:skip"} overrides {appendonly yes aof-use-rdb-preamble no}} {
    test {ZMOVE BYSCORE replays limited and unbounded batches from AOF} {
        r config set zset-max-listpack-entries 0
        r zadd src{t} 1 a 1 b 1 c 2 d
        r zadd dst{t} 99 a 100 retained
        r zmove src{t} dst{t} BYSCORE 1 1 LIMIT 2
        r zmove src{t} dst{t} BYSCORE (1 +inf
        set digest [r debug digest]
        r debug loadaof
        assert_equal $digest [r debug digest]
        assert_equal {c 1} [r zrange src{t} 0 -1 withscores]
        assert_equal {a 1 b 1 d 2 retained 100} [r zrange dst{t} 0 -1 withscores]
    }
}
