start_server {tags {"zset"}} {
    proc create_zset {key items} {
        r del $key
        foreach {score entry} $items {
            r zadd $key $score $entry
        }
    }

    test {ZSET basic ZADD and score update} {
        r zadd ztmp 10 x
        r zadd ztmp 20 y
        r zadd ztmp 30 z
        set aux1 [r zrange ztmp 0 -1]
        r zadd ztmp 1 y
        set aux2 [r zrange ztmp 0 -1]
        list $aux1 $aux2
    } {{x y z} {y x z}}

    test {ZCARD basics} {
        r zcard ztmp
    } {3}

    test {ZCARD non existing key} {
        r zcard ztmp-blabla
    } {0}

    test "ZRANGE basics" {
        r del ztmp
        r zadd ztmp 1 a
        r zadd ztmp 2 b
        r zadd ztmp 3 c
        r zadd ztmp 4 d

        assert_equal {a b c d} [r zrange ztmp 0 -1]
        assert_equal {a b c} [r zrange ztmp 0 -2]
        assert_equal {b c d} [r zrange ztmp 1 -1]
        assert_equal {b c} [r zrange ztmp 1 -2]
        assert_equal {c d} [r zrange ztmp -2 -1]
        assert_equal {c} [r zrange ztmp -2 -2]

        # out of range start index
        assert_equal {a b c} [r zrange ztmp -5 2]
        assert_equal {a b} [r zrange ztmp -5 1]
        assert_equal {} [r zrange ztmp 5 -1]
        assert_equal {} [r zrange ztmp 5 -2]

        # out of range end index
        assert_equal {a b c d} [r zrange ztmp 0 5]
        assert_equal {b c d} [r zrange ztmp 1 5]
        assert_equal {} [r zrange ztmp 0 -5]
        assert_equal {} [r zrange ztmp 1 -5]

        # withscores
        assert_equal {a 1 b 2 c 3 d 4} [r zrange ztmp 0 -1 withscores]
    }

    test "ZREVRANGE basics" {
        r del ztmp
        r zadd ztmp 1 a
        r zadd ztmp 2 b
        r zadd ztmp 3 c
        r zadd ztmp 4 d

        assert_equal {d c b a} [r zrevrange ztmp 0 -1]
        assert_equal {d c b} [r zrevrange ztmp 0 -2]
        assert_equal {c b a} [r zrevrange ztmp 1 -1]
        assert_equal {c b} [r zrevrange ztmp 1 -2]
        assert_equal {b a} [r zrevrange ztmp -2 -1]
        assert_equal {b} [r zrevrange ztmp -2 -2]

        # out of range start index
        assert_equal {d c b} [r zrevrange ztmp -5 2]
        assert_equal {d c} [r zrevrange ztmp -5 1]
        assert_equal {} [r zrevrange ztmp 5 -1]
        assert_equal {} [r zrevrange ztmp 5 -2]

        # out of range end index
        assert_equal {d c b a} [r zrevrange ztmp 0 5]
        assert_equal {c b a} [r zrevrange ztmp 1 5]
        assert_equal {} [r zrevrange ztmp 0 -5]
        assert_equal {} [r zrevrange ztmp 1 -5]

        # withscores
        assert_equal {d 4 c 3 b 2 a 1} [r zrevrange ztmp 0 -1 withscores]
    }

    test {ZRANK basics} {
        r zadd zranktmp 10 x
        r zadd zranktmp 20 y
        r zadd zranktmp 30 z
        list [r zrank zranktmp x] [r zrank zranktmp y] [r zrank zranktmp z]
    } {0 1 2}

    test {ZREVRANK basics} {
        list [r zrevrank zranktmp x] [r zrevrank zranktmp y] [r zrevrank zranktmp z]
    } {2 1 0}

    test {ZRANK - after deletion} {
        r zrem zranktmp y
        list [r zrank zranktmp x] [r zrank zranktmp z]
    } {0 1}

    test {ZSCORE} {
        set aux {}
        set err {}
        for {set i 0} {$i < 1000} {incr i} {
            set score [expr rand()]
            lappend aux $score
            r zadd zscoretest $score $i
        }
        for {set i 0} {$i < 1000} {incr i} {
            if {[r zscore zscoretest $i] != [lindex $aux $i]} {
                set err "Expected score was [lindex $aux $i] but got [r zscore zscoretest $i] for element $i"
                break
            }
        }
        set _ $err
    } {}

    test {ZSCORE after a DEBUG RELOAD} {
        set aux {}
        set err {}
        r del zscoretest
        for {set i 0} {$i < 1000} {incr i} {
            set score [expr rand()]
            lappend aux $score
            r zadd zscoretest $score $i
        }
        r debug reload
        for {set i 0} {$i < 1000} {incr i} {
            if {[r zscore zscoretest $i] != [lindex $aux $i]} {
                set err "Expected score was [lindex $aux $i] but got [r zscore zscoretest $i] for element $i"
                break
            }
        }
        set _ $err
    } {}

    test {ZSETs stress tester - sorting is working well?} {
        set delta 0
        for {set test 0} {$test < 2} {incr test} {
            unset -nocomplain auxarray
            array set auxarray {}
            set auxlist {}
            r del myzset
            for {set i 0} {$i < 1000} {incr i} {
                if {$test == 0} {
                    set score [expr rand()]
                } else {
                    set score [expr int(rand()*10)]
                }
                set auxarray($i) $score
                r zadd myzset $score $i
                # Random update
                if {[expr rand()] < .2} {
                    set j [expr int(rand()*1000)]
                    if {$test == 0} {
                        set score [expr rand()]
                    } else {
                        set score [expr int(rand()*10)]
                    }
                    set auxarray($j) $score
                    r zadd myzset $score $j
                }
            }
            foreach {item score} [array get auxarray] {
                lappend auxlist [list $score $item]
            }
            set sorted [lsort -command zlistAlikeSort $auxlist]
            set auxlist {}
            foreach x $sorted {
                lappend auxlist [lindex $x 1]
            }
            set fromredis [r zrange myzset 0 -1]
            set delta 0
            for {set i 0} {$i < [llength $fromredis]} {incr i} {
                if {[lindex $fromredis $i] != [lindex $auxlist $i]} {
                    incr delta
                }
            }
        }
        format $delta
    } {0}

    test {ZINCRBY - can create a new sorted set} {
        r del zset
        r zincrby zset 1 foo
        list [r zrange zset 0 -1] [r zscore zset foo]
    } {foo 1}

    test {ZINCRBY - increment and decrement} {
        r zincrby zset 2 foo
        r zincrby zset 1 bar
        set v1 [r zrange zset 0 -1]
        r zincrby zset 10 bar
        r zincrby zset -5 foo
        r zincrby zset -5 bar
        set v2 [r zrange zset 0 -1]
        list $v1 $v2 [r zscore zset foo] [r zscore zset bar]
    } {{bar foo} {foo bar} -2 6}

    proc create_default_zset {} {
        create_zset zset {-inf a 1 b 2 c 3 d 4 e 5 f +inf g}
    }

    test "ZRANGEBYSCORE/ZREVRANGEBYSCORE/ZCOUNT basics" {
        create_default_zset

        # inclusive range
        assert_equal {a b c} [r zrangebyscore zset -inf 2]
        assert_equal {b c d} [r zrangebyscore zset 0 3]
        assert_equal {d e f} [r zrangebyscore zset 3 6]
        assert_equal {e f g} [r zrangebyscore zset 4 +inf]
        assert_equal {c b a} [r zrevrangebyscore zset 2 -inf]
        assert_equal {d c b} [r zrevrangebyscore zset 3 0]
        assert_equal {f e d} [r zrevrangebyscore zset 6 3]
        assert_equal {g f e} [r zrevrangebyscore zset +inf 4]
        assert_equal 3 [r zcount zset 0 3]

        # exclusive range
        assert_equal {b}   [r zrangebyscore zset (-inf (2]
        assert_equal {b c} [r zrangebyscore zset (0 (3]
        assert_equal {e f} [r zrangebyscore zset (3 (6]
        assert_equal {f}   [r zrangebyscore zset (4 (+inf]
        assert_equal {b}   [r zrevrangebyscore zset (2 (-inf]
        assert_equal {c b} [r zrevrangebyscore zset (3 (0]
        assert_equal {f e} [r zrevrangebyscore zset (6 (3]
        assert_equal {f}   [r zrevrangebyscore zset (+inf (4]
        assert_equal 2 [r zcount zset (0 (3]
    }

    test "ZRANGEBYSCORE with WITHSCORES" {
        create_default_zset
        assert_equal {b 1 c 2 d 3} [r zrangebyscore zset 0 3 withscores]
        assert_equal {d 3 c 2 b 1} [r zrevrangebyscore zset 3 0 withscores]
    }

    test "ZRANGEBYSCORE with LIMIT" {
        create_default_zset
        assert_equal {b c}   [r zrangebyscore zset 0 10 LIMIT 0 2]
        assert_equal {d e f} [r zrangebyscore zset 0 10 LIMIT 2 3]
        assert_equal {d e f} [r zrangebyscore zset 0 10 LIMIT 2 10]
        assert_equal {}      [r zrangebyscore zset 0 10 LIMIT 20 10]
        assert_equal {f e}   [r zrevrangebyscore zset 10 0 LIMIT 0 2]
        assert_equal {d c b} [r zrevrangebyscore zset 10 0 LIMIT 2 3]
        assert_equal {d c b} [r zrevrangebyscore zset 10 0 LIMIT 2 10]
        assert_equal {}      [r zrevrangebyscore zset 10 0 LIMIT 20 10]
    }

    test "ZRANGEBYSCORE with LIMIT and WITHSCORES" {
        create_default_zset
        assert_equal {e 4 f 5} [r zrangebyscore zset 2 5 LIMIT 2 3 WITHSCORES]
        assert_equal {d 3 c 2} [r zrevrangebyscore zset 5 2 LIMIT 2 3 WITHSCORES]
    }

    test "ZRANGEBYSCORE with non-value min or max" {
        assert_error "*not a double*" {r zrangebyscore fooz str 1}
        assert_error "*not a double*" {r zrangebyscore fooz 1 str}
        assert_error "*not a double*" {r zrangebyscore fooz 1 NaN}
    }

    tags {"slow"} {
        test {ZRANGEBYSCORE fuzzy test, 100 ranges in 1000 elements sorted set} {
            set err {}
            r del zset
            for {set i 0} {$i < 1000} {incr i} {
                r zadd zset [expr rand()] $i
            }
            for {set i 0} {$i < 100} {incr i} {
                set min [expr rand()]
                set max [expr rand()]
                if {$min > $max} {
                    set aux $min
                    set min $max
                    set max $aux
                }
                set low [r zrangebyscore zset -inf $min]
                set ok [r zrangebyscore zset $min $max]
                set high [r zrangebyscore zset $max +inf]
                set lowx [r zrangebyscore zset -inf ($min]
                set okx [r zrangebyscore zset ($min ($max]
                set highx [r zrangebyscore zset ($max +inf]

                if {[r zcount zset -inf $min] != [llength $low]} {
                    append err "Error, len does not match zcount\n"
                }
                if {[r zcount zset $min $max] != [llength $ok]} {
                    append err "Error, len does not match zcount\n"
                }
                if {[r zcount zset $max +inf] != [llength $high]} {
                    append err "Error, len does not match zcount\n"
                }
                if {[r zcount zset -inf ($min] != [llength $lowx]} {
                    append err "Error, len does not match zcount\n"
                }
                if {[r zcount zset ($min ($max] != [llength $okx]} {
                    append err "Error, len does not match zcount\n"
                }
                if {[r zcount zset ($max +inf] != [llength $highx]} {
                    append err "Error, len does not match zcount\n"
                }

                foreach x $low {
                    set score [r zscore zset $x]
                    if {$score > $min} {
                        append err "Error, score for $x is $score > $min\n"
                    }
                }
                foreach x $lowx {
                    set score [r zscore zset $x]
                    if {$score >= $min} {
                        append err "Error, score for $x is $score >= $min\n"
                    }
                }
                foreach x $ok {
                    set score [r zscore zset $x]
                    if {$score < $min || $score > $max} {
                        append err "Error, score for $x is $score outside $min-$max range\n"
                    }
                }
                foreach x $okx {
                    set score [r zscore zset $x]
                    if {$score <= $min || $score >= $max} {
                        append err "Error, score for $x is $score outside $min-$max open range\n"
                    }
                }
                foreach x $high {
                    set score [r zscore zset $x]
                    if {$score < $max} {
                        append err "Error, score for $x is $score < $max\n"
                    }
                }
                foreach x $highx {
                    set score [r zscore zset $x]
                    if {$score <= $max} {
                        append err "Error, score for $x is $score <= $max\n"
                    }
                }
            }
            set _ $err
        } {}
    }

    test "ZREMRANGEBYSCORE basics" {
        proc remrangebyscore {min max} {
            create_zset zset {1 a 2 b 3 c 4 d 5 e}
            r zremrangebyscore zset $min $max
        }

        # inner range
        assert_equal 3 [remrangebyscore 2 4]
        assert_equal {a e} [r zrange zset 0 -1]

        # start underflow
        assert_equal 1 [remrangebyscore -10 1]
        assert_equal {b c d e} [r zrange zset 0 -1]

        # end overflow
        assert_equal 1 [remrangebyscore 5 10]
        assert_equal {a b c d} [r zrange zset 0 -1]

        # switch min and max
        assert_equal 0 [remrangebyscore 4 2]
        assert_equal {a b c d e} [r zrange zset 0 -1]

        # -inf to mid
        assert_equal 3 [remrangebyscore -inf 3]
        assert_equal {d e} [r zrange zset 0 -1]

        # mid to +inf
        assert_equal 3 [remrangebyscore 3 +inf]
        assert_equal {a b} [r zrange zset 0 -1]

        # -inf to +inf
        assert_equal 5 [remrangebyscore -inf +inf]
        assert_equal {} [r zrange zset 0 -1]

        # exclusive min
        assert_equal 4 [remrangebyscore (1 5]
        assert_equal {a} [r zrange zset 0 -1]
        assert_equal 3 [remrangebyscore (2 5]
        assert_equal {a b} [r zrange zset 0 -1]

        # exclusive max
        assert_equal 4 [remrangebyscore 1 (5]
        assert_equal {e} [r zrange zset 0 -1]
        assert_equal 3 [remrangebyscore 1 (4]
        assert_equal {d e} [r zrange zset 0 -1]

        # exclusive min and max
        assert_equal 3 [remrangebyscore (1 (5]
        assert_equal {a e} [r zrange zset 0 -1]
    }

    test "ZREMRANGEBYSCORE with non-value min or max" {
        assert_error "*not a double*" {r zremrangebyscore fooz str 1}
        assert_error "*not a double*" {r zremrangebyscore fooz 1 str}
        assert_error "*not a double*" {r zremrangebyscore fooz 1 NaN}
    }

    test "ZREMRANGEBYRANK basics" {
        proc remrangebyrank {min max} {
            create_zset zset {1 a 2 b 3 c 4 d 5 e}
            r zremrangebyrank zset $min $max
        }

        # inner range
        assert_equal 3 [remrangebyrank 1 3]
        assert_equal {a e} [r zrange zset 0 -1]

        # start underflow
        assert_equal 1 [remrangebyrank -10 0]
        assert_equal {b c d e} [r zrange zset 0 -1]

        # start overflow
        assert_equal 0 [remrangebyrank 10 -1]
        assert_equal {a b c d e} [r zrange zset 0 -1]

        # end underflow
        assert_equal 0 [remrangebyrank 0 -10]
        assert_equal {a b c d e} [r zrange zset 0 -1]

        # end overflow
        assert_equal 5 [remrangebyrank 0 10]
        assert_equal {} [r zrange zset 0 -1]
    }

    test {ZUNIONSTORE against non-existing key doesn't set destination} {
      r del zseta
      list [r zunionstore dst_key 1 zseta] [r exists dst_key]
    } {0 0}

    test {ZUNIONSTORE basics} {
        r del zseta zsetb zsetc
        r zadd zseta 1 a
        r zadd zseta 2 b
        r zadd zseta 3 c
        r zadd zsetb 1 b
        r zadd zsetb 2 c
        r zadd zsetb 3 d
        list [r zunionstore zsetc 2 zseta zsetb] [r zrange zsetc 0 -1 withscores]
    } {4 {a 1 b 3 d 3 c 5}}

    test {ZUNIONSTORE with weights} {
        list [r zunionstore zsetc 2 zseta zsetb weights 2 3] [r zrange zsetc 0 -1 withscores]
    } {4 {a 2 b 7 d 9 c 12}}

	test {ZUNIONSTORE with a regular set and weights} {
		r del seta
		r sadd seta a
		r sadd seta b
		r sadd seta c
        list [r zunionstore zsetc 2 seta zsetb weights 2 3] [r zrange zsetc 0 -1 withscores]
	} {4 {a 2 b 5 c 8 d 9}}

    test {ZUNIONSTORE with AGGREGATE MIN} {
        list [r zunionstore zsetc 2 zseta zsetb aggregate min] [r zrange zsetc 0 -1 withscores]
    } {4 {a 1 b 1 c 2 d 3}}

    test {ZUNIONSTORE with AGGREGATE MAX} {
        list [r zunionstore zsetc 2 zseta zsetb aggregate max] [r zrange zsetc 0 -1 withscores]
    } {4 {a 1 b 2 c 3 d 3}}

    test {ZINTERSTORE basics} {
        list [r zinterstore zsetc 2 zseta zsetb] [r zrange zsetc 0 -1 withscores]
    } {2 {b 3 c 5}}

    test {ZINTERSTORE with weights} {
        list [r zinterstore zsetc 2 zseta zsetb weights 2 3] [r zrange zsetc 0 -1 withscores]
    } {2 {b 7 c 12}}

	test {ZINTERSTORE with a regular set and weights} {
		r del seta
		r sadd seta a
		r sadd seta b
		r sadd seta c
        list [r zinterstore zsetc 2 seta zsetb weights 2 3] [r zrange zsetc 0 -1 withscores]
	} {2 {b 5 c 8}}

    test {ZINTERSTORE with AGGREGATE MIN} {
        list [r zinterstore zsetc 2 zseta zsetb aggregate min] [r zrange zsetc 0 -1 withscores]
    } {2 {b 1 c 2}}

    test {ZINTERSTORE with AGGREGATE MAX} {
        list [r zinterstore zsetc 2 zseta zsetb aggregate max] [r zrange zsetc 0 -1 withscores]
    } {2 {b 2 c 3}}
    
    foreach cmd {ZUNIONSTORE ZINTERSTORE} {
        test "$cmd with +inf/-inf scores" {
            r del zsetinf1 zsetinf2

            r zadd zsetinf1 +inf key
            r zadd zsetinf2 +inf key
            r $cmd zsetinf3 2 zsetinf1 zsetinf2
            assert_equal inf [r zscore zsetinf3 key]

            r zadd zsetinf1 -inf key
            r zadd zsetinf2 +inf key
            r $cmd zsetinf3 2 zsetinf1 zsetinf2
            assert_equal 0 [r zscore zsetinf3 key]

            r zadd zsetinf1 +inf key
            r zadd zsetinf2 -inf key
            r $cmd zsetinf3 2 zsetinf1 zsetinf2
            assert_equal 0 [r zscore zsetinf3 key]

            r zadd zsetinf1 -inf key
            r zadd zsetinf2 -inf key
            r $cmd zsetinf3 2 zsetinf1 zsetinf2
            assert_equal -inf [r zscore zsetinf3 key]
        }

        test "$cmd with NaN weights" {
            r del zsetinf1 zsetinf2

            r zadd zsetinf1 1.0 key
            r zadd zsetinf2 1.0 key
            assert_error "*weight value is not a double*" {
                r $cmd zsetinf3 2 zsetinf1 zsetinf2 weights nan nan
            }
        }
    }

    tags {"slow"} {
        test {ZSETs skiplist implementation backlink consistency test} {
            set diff 0
            set elements 10000
            for {set j 0} {$j < $elements} {incr j} {
                r zadd myzset [expr rand()] "Element-$j"
                r zrem myzset "Element-[expr int(rand()*$elements)]"
            }
            set l1 [r zrange myzset 0 -1]
            set l2 [r zrevrange myzset 0 -1]
            for {set j 0} {$j < [llength $l1]} {incr j} {
                if {[lindex $l1 $j] ne [lindex $l2 end-$j]} {
                    incr diff
                }
            }
            format $diff
        } {0}

        test {ZSETs ZRANK augmented skip list stress testing} {
            set err {}
            r del myzset
            for {set k 0} {$k < 10000} {incr k} {
                set i [expr {$k%1000}]
                if {[expr rand()] < .2} {
                    r zrem myzset $i
                } else {
                    set score [expr rand()]
                    r zadd myzset $score $i
                }
                set card [r zcard myzset]
                if {$card > 0} {
                    set index [randomInt $card]
                    set ele [lindex [r zrange myzset $index $index] 0]
                    set rank [r zrank myzset $ele]
                    if {$rank != $index} {
                        set err "$ele RANK is wrong! ($rank != $index)"
                        break
                    }
                }
            }
            set _ $err
        } {}
    }

    test {ZSET element can't be set to NaN with ZADD} {
        assert_error "*not a double*" {r zadd myzset nan abc}
    }

    test {ZSET element can't be set to NaN with ZINCRBY} {
        assert_error "*not a double*" {r zadd myzset nan abc}
    }

    test {ZINCRBY calls leading to NaN result in error} {
        r zincrby myzset +inf abc
        assert_error "*NaN*" {r zincrby myzset -inf abc}
    }
}
