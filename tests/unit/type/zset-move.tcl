# Sorted-set moves: single members, blocking moves and score-range batches.
start_server {tags {"zset needs:debug"} overrides {key-memory-histograms yes}} {
    proc create_move_zset {key items} {
        r del $key
        foreach {score entry} $items {
            r zadd $key $score $entry
        }
    }

    proc verify_move_nil_response {resp nil_response} {
        assert_equal [expr {$resp == 2 ? "*-1" : "_"}] $nil_response
    }

    proc verify_move_score_response {rd resp score} {
        if {$resp == 2} {
            assert_equal {$1} [$rd read]
            assert_equal $score [$rd read]
        } else {
            assert_equal ",$score" [$rd read]
        }
    }

    r debug allocsize-slots-assert 1
    foreach encoding {listpack btree} {
        r config set zset-max-listpack-entries [expr {$encoding eq "listpack" ? 128 : 0}]
        r config set zset-max-listpack-value [expr {$encoding eq "listpack" ? 64 : 0}]
        foreach position {min max} {
            test "ZMOVE $position - $encoding" {
                r del target{t}
                create_move_zset zset{t} {-1 a 1 b 2 c 3 d}

                if {$position eq "min"} {
                    assert_equal {a -1} [r zmove zset{t} target{t} $position]
                    assert_equal {b c d} [r zrange zset{t} 0 -1]
                    assert_equal {a -1} [r zrange target{t} 0 -1 withscores]
                } else {
                    assert_equal {d 3} [r zmove zset{t} target{t} $position]
                    assert_equal {a b c} [r zrange zset{t} 0 -1]
                    assert_equal {d 3} [r zrange target{t} 0 -1 withscores]
                }
            }

            test "BZMOVE $position non-blocking path - $encoding" {
                r del target{t}
                create_move_zset zset{t} {-1 a 1 b 2 c 3 d}

                if {$position eq "min"} {
                    assert_equal {a -1} [r bzmove zset{t} target{t} $position 1]
                    assert_equal {b c d} [r zrange zset{t} 0 -1]
                    assert_equal {a -1} [r zrange target{t} 0 -1 withscores]
                } else {
                    assert_equal {d 3} [r bzmove zset{t} target{t} $position 1]
                    assert_equal {a b c} [r zrange zset{t} 0 -1]
                    assert_equal {d 3} [r zrange target{t} 0 -1 withscores]
                }
            }

            test "ZMOVE $position with the same zset as source and destination - $encoding" {
                create_move_zset zset{t} {-1 a 1 b 2 c 3 d}
                assert_equal {} [r zmove zset{t} zset{t} $position]
                assert_equal {a b c d} [r zrange zset{t} 0 -1]
            }
        }

        test "ZMOVEMEMBER - $encoding" {
            r del target{t}
            create_move_zset zset{t} {-1 a 1 b 2 c 3 d}

            assert_equal {b 1} [r zmovemember zset{t} target{t} b]
            assert_equal {b 1} [r zrange target{t} 0 -1 withscores]
            assert_equal {a c d} [r zrange zset{t} 0 -1]
        }

        test "ZMOVEMEMBER updates existing destination member - $encoding" {
            create_move_zset zset{t} {-1 a 1 b 2 c 3 d}
            r del target{t}
            r zadd target{t} 42 b

            assert_equal {b 1} [r zmovemember zset{t} target{t} b]
            assert_equal {b 1} [r zrange target{t} 0 -1 withscores]
            assert_equal {a c d} [r zrange zset{t} 0 -1]
        }

        test "ZMOVE preserves binary members and deletes the last source member - $encoding" {
            set member "a\x00\x62[string repeat x 80]"
            r del zset{t} target{t}
            r zadd zset{t} -1 $member
            assert_encoding btree zset{t}
            assert_equal [list $member -1] [r zmove zset{t} target{t} min]
            assert_equal 0 [r exists zset{t}]
            assert_equal [list $member -1] [r zrange target{t} 0 -1 withscores]
        }

        test "ZMOVEMEMBER with missing member or same source and destination - $encoding" {
            create_move_zset zset{t} {-1 a 1 b 2 c 3 d}
            r del target{t}

            assert_equal {} [r zmovemember zset{t} target{t} missing]
            assert_equal {} [r zmovemember zset{t} zset{t} a]
            assert_equal {a b c d} [r zrange zset{t} 0 -1]
            assert_equal 0 [r exists target{t}]
        }
    }
    r config set zset-max-listpack-entries 128
    r config set zset-max-listpack-value 64

    test {ZMOVE/BZMOVE/ZMOVEMEMBER reject wrong types and invalid arguments} {
        r del source{t} target{t} wrong{t}
        r set wrong{t} string
        r zadd source{t} 1 a
        foreach args {{zmove min} {bzmove max 1} {zmovemember a}} {
            set command [lindex $args 0]
            set selection [lrange $args 1 end]
            assert_error {*WRONGTYPE*} {r $command wrong{t} target{t} {*}$selection}
            assert_error {*WRONGTYPE*} {r $command source{t} wrong{t} {*}$selection}
        }
        assert_error {ERR syntax error*} {r zmove source{t} target{t} middle}
        assert_error {ERR syntax error*} {r bzmove source{t} target{t} middle 1}
        assert_error {ERR timeout*} {r bzmove source{t} target{t} min invalid-timeout}
        assert_equal {a 1} [r zrange source{t} 0 -1 withscores]
        assert_equal string [r get wrong{t}]
        assert_equal 0 [r exists target{t}]
    }

    foreach resp {2 3} {
        if {$resp == 3 && [lsearch $::denytags "resp3"] >= 0} {continue}
        if {$resp == 2 && $::force_resp3} {continue}
        r hello $resp
        set rd [redis_deferring_client]
        $rd hello $resp
        $rd read
        test "ZMOVE/BZMOVE/ZMOVEMEMBER readraw in RESP$resp" {
            r del missing{t} target{t}
            create_move_zset zset2{t} {1 a 2 b 3 c 4 d 5 e}

            r readraw 1
            $rd readraw 1

            verify_move_nil_response $resp [r zmove missing{t} target{t} min]
            verify_move_nil_response $resp [r zmovemember missing{t} target{t} a]

            assert_equal {*2} [r zmove zset2{t} target{t} min]
            assert_equal [r read] {$1}
            assert_equal [r read] {a}
            verify_move_score_response r $resp 1

            assert_equal {*2} [r zmovemember zset2{t} target{t} b]
            assert_equal [r read] {$1}
            assert_equal [r read] {b}
            verify_move_score_response r $resp 2

            $rd bzmove missing{t} target{t} max 0.01
            verify_move_nil_response $resp [$rd read]

            $rd bzmove zset2{t} target{t} max 0.01
            assert_equal [$rd read] {*2}
            assert_equal [$rd read] {$1}
            assert_equal [$rd read] {e}
            verify_move_score_response $rd $resp 5

            r readraw 0
            $rd readraw 0
        }

        $rd close
        r hello 2
    }

    foreach position {min max} {
        test "BZMOVE $position with timeout" {
            r del zset{t} target{t}
            set rd [redis_deferring_client]
            $rd bzmove zset{t} target{t} $position 0.1
            wait_for_blocked_client
            # Bound the wait so a deadline in the wrong clock domain fails
            # promptly rather than leaving the test client blocked indefinitely.
            wait_for_condition 100 10 {
                [s blocked_clients] == 0
            } else {
                $rd close
                fail "BZMOVE did not time out"
            }
            assert_equal {} [$rd read]
            assert_equal 0 [r exists target{t}]
            $rd close
        }

        test "BZMOVE $position with zero timeout should block indefinitely" {
            set rd [redis_deferring_client]
            r del zset{t} target{t}
            $rd bzmove zset{t} target{t} $position 0
            wait_for_blocked_client
            r zadd zset{t} 0 foo
            assert_equal {foo 0} [$rd read]
            assert_equal {foo 0} [r zrange target{t} 0 -1 withscores]
            $rd close
        }

        test "BZMOVE $position with multiple blocked clients" {
            set rd1 [redis_deferring_client]
            set rd2 [redis_deferring_client]
            set rd3 [redis_deferring_client]
            r del zset{t} target{t}

            $rd1 bzmove zset{t} target{t} $position 0
            wait_for_blocked_clients_count 1
            $rd2 bzmove zset{t} target{t} $position 0
            wait_for_blocked_clients_count 2
            $rd3 bzmove zset{t} target{t} $position 0
            wait_for_blocked_clients_count 3

            r zadd zset{t} -1 a 1 b 2 c 3 d

            if {$position eq "min"} {
                assert_equal {a -1} [$rd1 read]
                assert_equal {b 1} [$rd2 read]
                assert_equal {c 2} [$rd3 read]
            } else {
                assert_equal {d 3} [$rd1 read]
                assert_equal {c 2} [$rd2 read]
                assert_equal {b 1} [$rd3 read]
            }

            $rd1 close
            $rd2 close
            $rd3 close
        }
    }

    test "BZMOVE with the same source and destination should not block" {
        r del zset{t}
        set start [clock milliseconds]
        assert_equal {} [r bzmove zset{t} zset{t} min 5]
        assert_lessthan [expr {[clock milliseconds]-$start}] 1000
    }

    test "BZMOVE inside MULTI should not block" {
        r del zset{t} target{t}
        r readraw 1

        assert_equal {+OK} [r multi]
        assert_equal {+QUEUED} [r bzmove zset{t} target{t} min 0]
        assert_equal {*1} [r exec]
        verify_move_nil_response [expr {$::force_resp3 ? 3 : 2}] [r read]

        r readraw 0
        assert_equal 0 [r exists target{t}]
    }

    test "BZMOVE should not block on the destination key" {
        set rd [redis_deferring_client]
        r del source{t} target{t}
        $rd bzmove source{t} target{t} min 0
        wait_for_blocked_client

        r zadd target{t} 1 target_member
        wait_for_blocked_client

        r zadd source{t} 2 source_member
        assert_equal {source_member 2} [$rd read]
        assert_equal {target_member 1 source_member 2} [r zrange target{t} 0 -1 withscores]
        $rd close
    }

    test "BZMOVE compares source and destination keys binary-safely" {
        set rd [redis_deferring_client]
        r del source{t} SOURCE{t}
        $rd bzmove source{t} SOURCE{t} min 0
        wait_for_blocked_client

        r zadd source{t} 1 member
        assert_equal {member 1} [$rd read]
        assert_equal {member 1} [r zrange SOURCE{t} 0 -1 withscores]
        $rd close
    }

    test "BZMOVE with wrong destination type while blocked" {
        set rd [redis_deferring_client]
        r del source{t} target{t}
        r set target{t} notzset
        $rd bzmove source{t} target{t} min 0
        wait_for_blocked_clients_count 1

        # The destination type is checked before the source is modified.
        r zadd source{t} 1 a 2 b
        assert_error "WRONGTYPE*" {$rd read}
        assert_equal {a 1 b 2} [r zrange source{t} 0 -1 withscores]
        assert_equal {notzset} [r get target{t}]
        $rd close
    }

}

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

    foreach command {zmove bzmove} {
        foreach position {min max} {
            test "$command $position moves integer-encoded listpack members" {
                r del src{t} dst{t}
                r config set zset-max-listpack-entries 128
                r zadd src{t} 1 42 1 7
                assert_encoding listpack src{t}
                # Equal scores still use lexical member ordering, including
                # when listpack stores the member bytes as integers.
                if {$position eq "min"} {
                    set moved 42
                    set remaining 7
                } else {
                    set moved 7
                    set remaining 42
                }
                r zadd dst{t} 99 $moved 100 retained
                set args [list $position]
                if {$command eq "bzmove"} {lappend args 0}
                assert_equal [list $moved 1] [r $command src{t} dst{t} {*}$args]
                assert_equal [list $remaining 1] [r zrange src{t} 0 -1 withscores]
                assert_equal [list $moved 1 retained 100] [r zrange dst{t} 0 -1 withscores]
            }
        }
    }

    foreach dst_encoding {listpack btree} {
        test "ZMOVE BYSCORE preserves signed integer listpack members - $dst_encoding destination" {
            r del src{t} dst{t}
            r config set zset-max-listpack-entries 128
            set members {-9223372036854775808 -1 0 127 128 32767 32768 9223372036854775807}
            foreach member $members {r zadd src{t} 1 $member}
            assert_encoding listpack src{t}
            r config set zset-max-listpack-entries [expr {$dst_encoding eq "listpack" ? 128 : 0}]
            r zadd dst{t} 99 -1 100 retained
            assert_encoding $dst_encoding dst{t}
            set expected {}
            foreach member [lsort -ascii $members] {lappend expected [list $member 1]}
            assert_equal [lrange $expected 0 2] [r zmove src{t} dst{t} BYSCORE 1 1 LIMIT 3]
            set remaining {}
            foreach pair [lrange $expected 3 end] {lappend remaining {*}$pair}
            assert_equal $remaining [r zrange src{t} 0 -1 withscores]
            assert_equal [lrange $expected 3 end] [r zmove src{t} dst{t} BYSCORE -inf +inf]
            set flattened {}
            foreach pair $expected {lappend flattened {*}$pair}
            assert_equal [concat $flattened {retained 100}] [r zrange dst{t} 0 -1 withscores]
            assert_equal 0 [r exists src{t}]
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
        if {$resp == 3 && [lsearch $::denytags "resp3"] >= 0} {continue}
        if {$resp == 2 && $::force_resp3} {continue}
        test "ZMOVE BYSCORE returns nested pairs including integer and binary members in RESP$resp" {
            r hello $resp
            r del src{t} dst{t}
            r config set zset-max-listpack-entries 128
            set binary "a\x00\x62"
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

start_server {tags {"zset needs:debug"} overrides {key-memory-histograms yes zset-max-listpack-entries 0}} {
    test {ZMOVEMEMBER tracks memory during B+tree index rehash} {
        r debug allocsize-slots-assert 1
        for {set i 0} {$i < 100} {incr i} {
            r zadd source{t} $i m$i
        }
        assert_encoding btree source{t}
        for {set i 0} {$i < 100} {incr i} {
            # Both successful and missing-member lookups may finish a rehash.
            assert_equal {} [r zmovemember source{t} target{t} missing]
            assert_equal [list m$i $i] [r zmovemember source{t} target{t} m$i]
        }
        assert_equal 0 [r exists source{t}]
        assert_equal 100 [r zcard target{t}]
        r debug allocsize-slots-assert 0
    }
}
