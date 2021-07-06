start_server {tags {"zset"}} {
    proc create_zset {key items} {
        r del $key
        foreach {score entry} $items {
            r zadd $key $score $entry
        }
    }

    proc basics {encoding} {
        set original_max_entries [lindex [r config get zset-max-ziplist-entries] 1]
        set original_max_value [lindex [r config get zset-max-ziplist-value] 1]
        if {$encoding == "ziplist"} {
            r config set zset-max-ziplist-entries 128
            r config set zset-max-ziplist-value 64
        } elseif {$encoding == "skiplist"} {
            r config set zset-max-ziplist-entries 0
            r config set zset-max-ziplist-value 0
        } else {
            puts "Unknown sorted set encoding"
            exit
        }

        test "Check encoding - $encoding" {
            r del ztmp
            r zadd ztmp 10 x
            assert_encoding $encoding ztmp
        }

        test "ZSET basic ZADD and score update - $encoding" {
            r del ztmp
            r zadd ztmp 10 x
            r zadd ztmp 20 y
            r zadd ztmp 30 z
            assert_equal {x y z} [r zrange ztmp 0 -1]

            r zadd ztmp 1 y
            assert_equal {y x z} [r zrange ztmp 0 -1]
        }

        test "ZSET element can't be set to NaN with ZADD - $encoding" {
            assert_error "*not*float*" {r zadd myzset nan abc}
        }

        test "ZSET element can't be set to NaN with ZINCRBY - $encoding" {
            assert_error "*not*float*" {r zadd myzset nan abc}
        }

        test "ZADD with options syntax error with incomplete pair - $encoding" {
            r del ztmp
            catch {r zadd ztmp xx 10 x 20} err
            set err
        } {ERR*}

        test "ZADD XX option without key - $encoding" {
            r del ztmp
            assert {[r zadd ztmp xx 10 x] == 0}
            assert {[r type ztmp] eq {none}}
        }

        test "ZADD XX existing key - $encoding" {
            r del ztmp
            r zadd ztmp 10 x
            assert {[r zadd ztmp xx 20 y] == 0}
            assert {[r zcard ztmp] == 1}
        }

        test "ZADD XX returns the number of elements actually added - $encoding" {
            r del ztmp
            r zadd ztmp 10 x
            set retval [r zadd ztmp 10 x 20 y 30 z]
            assert {$retval == 2}
        }

        test "ZADD XX updates existing elements score - $encoding" {
            r del ztmp
            r zadd ztmp 10 x 20 y 30 z
            r zadd ztmp xx 5 foo 11 x 21 y 40 zap
            assert {[r zcard ztmp] == 3}
            assert {[r zscore ztmp x] == 11}
            assert {[r zscore ztmp y] == 21}
        }

        test "ZADD GT updates existing elements when new scores are greater - $encoding" {
            r del ztmp
            r zadd ztmp 10 x 20 y 30 z
            assert {[r zadd ztmp gt ch 5 foo 11 x 21 y 29 z] == 3}
            assert {[r zcard ztmp] == 4}
            assert {[r zscore ztmp x] == 11}
            assert {[r zscore ztmp y] == 21}
            assert {[r zscore ztmp z] == 30}
        }

        test "ZADD LT updates existing elements when new scores are lower - $encoding" {
            r del ztmp
            r zadd ztmp 10 x 20 y 30 z
            assert {[r zadd ztmp lt ch 5 foo 11 x 21 y 29 z] == 2}
            assert {[r zcard ztmp] == 4}
            assert {[r zscore ztmp x] == 10}
            assert {[r zscore ztmp y] == 20}
            assert {[r zscore ztmp z] == 29}
        }

        test "ZADD GT XX updates existing elements when new scores are greater and skips new elements - $encoding" {
            r del ztmp
            r zadd ztmp 10 x 20 y 30 z
            assert {[r zadd ztmp gt xx ch 5 foo 11 x 21 y 29 z] == 2}
            assert {[r zcard ztmp] == 3}
            assert {[r zscore ztmp x] == 11}
            assert {[r zscore ztmp y] == 21}
            assert {[r zscore ztmp z] == 30}
        }

        test "ZADD LT XX updates existing elements when new scores are lower and skips new elements - $encoding" {
            r del ztmp
            r zadd ztmp 10 x 20 y 30 z
            assert {[r zadd ztmp lt xx ch 5 foo 11 x 21 y 29 z] == 1}
            assert {[r zcard ztmp] == 3}
            assert {[r zscore ztmp x] == 10}
            assert {[r zscore ztmp y] == 20}
            assert {[r zscore ztmp z] == 29}
        }

        test "ZADD XX and NX are not compatible - $encoding" {
            r del ztmp
            catch {r zadd ztmp xx nx 10 x} err
            set err
        } {ERR*}

        test "ZADD NX with non existing key - $encoding" {
            r del ztmp
            r zadd ztmp nx 10 x 20 y 30 z
            assert {[r zcard ztmp] == 3}
        }

        test "ZADD NX only add new elements without updating old ones - $encoding" {
            r del ztmp
            r zadd ztmp 10 x 20 y 30 z
            assert {[r zadd ztmp nx 11 x 21 y 100 a 200 b] == 2}
            assert {[r zscore ztmp x] == 10}
            assert {[r zscore ztmp y] == 20}
            assert {[r zscore ztmp a] == 100}
            assert {[r zscore ztmp b] == 200}
        }

        test "ZADD GT and NX are not compatible - $encoding" {
            r del ztmp
            catch {r zadd ztmp gt nx 10 x} err
            set err
        } {ERR*}

        test "ZADD LT and NX are not compatible - $encoding" {
            r del ztmp
            catch {r zadd ztmp lt nx 10 x} err
            set err
        } {ERR*}

        test "ZADD LT and GT are not compatible - $encoding" {
            r del ztmp
            catch {r zadd ztmp lt gt 10 x} err
            set err
        } {ERR*}

        test "ZADD INCR LT/GT replies with nill if score not updated - $encoding" {
            r del ztmp
            r zadd ztmp 28 x
            assert {[r zadd ztmp lt incr 1 x] eq {}}
            assert {[r zscore ztmp x] == 28}
            assert {[r zadd ztmp gt incr -1 x] eq {}}
            assert {[r zscore ztmp x] == 28}
        }

        test "ZADD INCR LT/GT with inf - $encoding" {
            r del ztmp
            r zadd ztmp +inf x -inf y

            assert {[r zadd ztmp lt incr 1 x] eq {}}
            assert {[r zscore ztmp x] == inf}
            assert {[r zadd ztmp gt incr -1 x] eq {}}
            assert {[r zscore ztmp x] == inf}
            assert {[r zadd ztmp lt incr -1 x] eq {}}
            assert {[r zscore ztmp x] == inf}
            assert {[r zadd ztmp gt incr 1 x] eq {}}
            assert {[r zscore ztmp x] == inf}

            assert {[r zadd ztmp lt incr 1 y] eq {}}
            assert {[r zscore ztmp y] == -inf}
            assert {[r zadd ztmp gt incr -1 y] eq {}}
            assert {[r zscore ztmp y] == -inf}
            assert {[r zadd ztmp lt incr -1 y] eq {}}
            assert {[r zscore ztmp y] == -inf}
            assert {[r zadd ztmp gt incr 1 y] eq {}}
            assert {[r zscore ztmp y] == -inf}
        }

        test "ZADD INCR works like ZINCRBY - $encoding" {
            r del ztmp
            r zadd ztmp 10 x 20 y 30 z
            r zadd ztmp INCR 15 x
            assert {[r zscore ztmp x] == 25}
        }

        test "ZADD INCR works with a single score-elemenet pair - $encoding" {
            r del ztmp
            r zadd ztmp 10 x 20 y 30 z
            catch {r zadd ztmp INCR 15 x 10 y} err
            set err
        } {ERR*}

        test "ZADD CH option changes return value to all changed elements - $encoding" {
            r del ztmp
            r zadd ztmp 10 x 20 y 30 z
            assert {[r zadd ztmp 11 x 21 y 30 z] == 0}
            assert {[r zadd ztmp ch 12 x 22 y 30 z] == 2}
        }

        test "ZINCRBY calls leading to NaN result in error - $encoding" {
            r zincrby myzset +inf abc
            assert_error "*NaN*" {r zincrby myzset -inf abc}
        }

        test {ZADD - Variadic version base case - $encoding} {
            r del myzset
            list [r zadd myzset 10 a 20 b 30 c] [r zrange myzset 0 -1 withscores]
        } {3 {a 10 b 20 c 30}}

        test {ZADD - Return value is the number of actually added items - $encoding} {
            list [r zadd myzset 5 x 20 b 30 c] [r zrange myzset 0 -1 withscores]
        } {1 {x 5 a 10 b 20 c 30}}

        test {ZADD - Variadic version does not add nothing on single parsing err - $encoding} {
            r del myzset
            catch {r zadd myzset 10 a 20 b 30.badscore c} e
            assert_match {*ERR*not*float*} $e
            r exists myzset
        } {0}

        test {ZADD - Variadic version will raise error on missing arg - $encoding} {
            r del myzset
            catch {r zadd myzset 10 a 20 b 30 c 40} e
            assert_match {*ERR*syntax*} $e
        }

        test {ZINCRBY does not work variadic even if shares ZADD implementation - $encoding} {
            r del myzset
            catch {r zincrby myzset 10 a 20 b 30 c} e
            assert_match {*ERR*wrong*number*arg*} $e
        }

        test "ZCARD basics - $encoding" {
            r del ztmp
            r zadd ztmp 10 a 20 b 30 c
            assert_equal 3 [r zcard ztmp]
            assert_equal 0 [r zcard zdoesntexist]
        }

        test "ZREM removes key after last element is removed - $encoding" {
            r del ztmp
            r zadd ztmp 10 x
            r zadd ztmp 20 y

            assert_equal 1 [r exists ztmp]
            assert_equal 0 [r zrem ztmp z]
            assert_equal 1 [r zrem ztmp y]
            assert_equal 1 [r zrem ztmp x]
            assert_equal 0 [r exists ztmp]
        }

        test "ZREM variadic version - $encoding" {
            r del ztmp
            r zadd ztmp 10 a 20 b 30 c
            assert_equal 2 [r zrem ztmp x y a b k]
            assert_equal 0 [r zrem ztmp foo bar]
            assert_equal 1 [r zrem ztmp c]
            r exists ztmp
        } {0}

        test "ZREM variadic version -- remove elements after key deletion - $encoding" {
            r del ztmp
            r zadd ztmp 10 a 20 b 30 c
            r zrem ztmp a b c d e f g
        } {3}

        test "ZRANGE basics - $encoding" {
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

        test "ZREVRANGE basics - $encoding" {
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

        test "ZRANK/ZREVRANK basics - $encoding" {
            r del zranktmp
            r zadd zranktmp 10 x
            r zadd zranktmp 20 y
            r zadd zranktmp 30 z
            assert_equal 0 [r zrank zranktmp x]
            assert_equal 1 [r zrank zranktmp y]
            assert_equal 2 [r zrank zranktmp z]
            assert_equal "" [r zrank zranktmp foo]
            assert_equal 2 [r zrevrank zranktmp x]
            assert_equal 1 [r zrevrank zranktmp y]
            assert_equal 0 [r zrevrank zranktmp z]
            assert_equal "" [r zrevrank zranktmp foo]
        }

        test "ZRANK - after deletion - $encoding" {
            r zrem zranktmp y
            assert_equal 0 [r zrank zranktmp x]
            assert_equal 1 [r zrank zranktmp z]
        }

        test "ZINCRBY - can create a new sorted set - $encoding" {
            r del zset
            r zincrby zset 1 foo
            assert_equal {foo} [r zrange zset 0 -1]
            assert_equal 1 [r zscore zset foo]
        }

        test "ZINCRBY - increment and decrement - $encoding" {
            r zincrby zset 2 foo
            r zincrby zset 1 bar
            assert_equal {bar foo} [r zrange zset 0 -1]

            r zincrby zset 10 bar
            r zincrby zset -5 foo
            r zincrby zset -5 bar
            assert_equal {foo bar} [r zrange zset 0 -1]

            assert_equal -2 [r zscore zset foo]
            assert_equal  6 [r zscore zset bar]
        }

        test "ZINCRBY return value - $encoding" {
            r del ztmp
            set retval [r zincrby ztmp 1.0 x]
            assert {$retval == 1.0}
        }

        proc create_default_zset {} {
            create_zset zset {-inf a 1 b 2 c 3 d 4 e 5 f +inf g}
        }

        test "ZRANGEBYSCORE/ZREVRANGEBYSCORE/ZCOUNT basics - $encoding" {
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

            # test empty ranges
            r zrem zset a
            r zrem zset g

            # inclusive
            assert_equal {} [r zrangebyscore zset 4 2]
            assert_equal {} [r zrangebyscore zset 6 +inf]
            assert_equal {} [r zrangebyscore zset -inf -6]
            assert_equal {} [r zrevrangebyscore zset +inf 6]
            assert_equal {} [r zrevrangebyscore zset -6 -inf]

            # exclusive
            assert_equal {} [r zrangebyscore zset (4 (2]
            assert_equal {} [r zrangebyscore zset 2 (2]
            assert_equal {} [r zrangebyscore zset (2 2]
            assert_equal {} [r zrangebyscore zset (6 (+inf]
            assert_equal {} [r zrangebyscore zset (-inf (-6]
            assert_equal {} [r zrevrangebyscore zset (+inf (6]
            assert_equal {} [r zrevrangebyscore zset (-6 (-inf]

            # empty inner range
            assert_equal {} [r zrangebyscore zset 2.4 2.6]
            assert_equal {} [r zrangebyscore zset (2.4 2.6]
            assert_equal {} [r zrangebyscore zset 2.4 (2.6]
            assert_equal {} [r zrangebyscore zset (2.4 (2.6]
        }

        test "ZRANGEBYSCORE with WITHSCORES - $encoding" {
            create_default_zset
            assert_equal {b 1 c 2 d 3} [r zrangebyscore zset 0 3 withscores]
            assert_equal {d 3 c 2 b 1} [r zrevrangebyscore zset 3 0 withscores]
        }

        test "ZRANGEBYSCORE with LIMIT - $encoding" {
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

        test "ZRANGEBYSCORE with LIMIT and WITHSCORES - $encoding" {
            create_default_zset
            assert_equal {e 4 f 5} [r zrangebyscore zset 2 5 LIMIT 2 3 WITHSCORES]
            assert_equal {d 3 c 2} [r zrevrangebyscore zset 5 2 LIMIT 2 3 WITHSCORES]
            assert_equal {} [r zrangebyscore zset 2 5 LIMIT 12 13 WITHSCORES]
        }

        test "ZRANGEBYSCORE with non-value min or max - $encoding" {
            assert_error "*not*float*" {r zrangebyscore fooz str 1}
            assert_error "*not*float*" {r zrangebyscore fooz 1 str}
            assert_error "*not*float*" {r zrangebyscore fooz 1 NaN}
        }

        proc create_default_lex_zset {} {
            create_zset zset {0 alpha 0 bar 0 cool 0 down
                              0 elephant 0 foo 0 great 0 hill
                              0 omega}
        }

        test "ZRANGEBYLEX/ZREVRANGEBYLEX/ZLEXCOUNT basics - $encoding" {
            create_default_lex_zset

            # inclusive range
            assert_equal {alpha bar cool} [r zrangebylex zset - \[cool]
            assert_equal {bar cool down} [r zrangebylex zset \[bar \[down]
            assert_equal {great hill omega} [r zrangebylex zset \[g +]
            assert_equal {cool bar alpha} [r zrevrangebylex zset \[cool -]
            assert_equal {down cool bar} [r zrevrangebylex zset \[down \[bar]
            assert_equal {omega hill great foo elephant down} [r zrevrangebylex zset + \[d]
            assert_equal 3 [r zlexcount zset \[ele \[h]

            # exclusive range
            assert_equal {alpha bar} [r zrangebylex zset - (cool]
            assert_equal {cool} [r zrangebylex zset (bar (down]
            assert_equal {hill omega} [r zrangebylex zset (great +]
            assert_equal {bar alpha} [r zrevrangebylex zset (cool -]
            assert_equal {cool} [r zrevrangebylex zset (down (bar]
            assert_equal {omega hill} [r zrevrangebylex zset + (great]
            assert_equal 2 [r zlexcount zset (ele (great]

            # inclusive and exclusive
            assert_equal {} [r zrangebylex zset (az (b]
            assert_equal {} [r zrangebylex zset (z +]
            assert_equal {} [r zrangebylex zset - \[aaaa]
            assert_equal {} [r zrevrangebylex zset \[elez \[elex]
            assert_equal {} [r zrevrangebylex zset (hill (omega]
        }
        
        test "ZLEXCOUNT advanced - $encoding" {
            create_default_lex_zset
    
            assert_equal 9 [r zlexcount zset - +]
            assert_equal 0 [r zlexcount zset + -]
            assert_equal 0 [r zlexcount zset + \[c]
            assert_equal 0 [r zlexcount zset \[c -]
            assert_equal 8 [r zlexcount zset \[bar +]
            assert_equal 5 [r zlexcount zset \[bar \[foo]
            assert_equal 4 [r zlexcount zset \[bar (foo]
            assert_equal 4 [r zlexcount zset (bar \[foo]
            assert_equal 3 [r zlexcount zset (bar (foo]
            assert_equal 5 [r zlexcount zset - (foo]
            assert_equal 1 [r zlexcount zset (maxstring +]
        }

        test "ZRANGEBYSLEX with LIMIT - $encoding" {
            create_default_lex_zset
            assert_equal {alpha bar} [r zrangebylex zset - \[cool LIMIT 0 2]
            assert_equal {bar cool} [r zrangebylex zset - \[cool LIMIT 1 2]
            assert_equal {} [r zrangebylex zset \[bar \[down LIMIT 0 0]
            assert_equal {} [r zrangebylex zset \[bar \[down LIMIT 2 0]
            assert_equal {bar} [r zrangebylex zset \[bar \[down LIMIT 0 1]
            assert_equal {cool} [r zrangebylex zset \[bar \[down LIMIT 1 1]
            assert_equal {bar cool down} [r zrangebylex zset \[bar \[down LIMIT 0 100]
            assert_equal {omega hill great foo elephant} [r zrevrangebylex zset + \[d LIMIT 0 5]
            assert_equal {omega hill great foo} [r zrevrangebylex zset + \[d LIMIT 0 4]
        }

        test "ZRANGEBYLEX with invalid lex range specifiers - $encoding" {
            assert_error "*not*string*" {r zrangebylex fooz foo bar}
            assert_error "*not*string*" {r zrangebylex fooz \[foo bar}
            assert_error "*not*string*" {r zrangebylex fooz foo \[bar}
            assert_error "*not*string*" {r zrangebylex fooz +x \[bar}
            assert_error "*not*string*" {r zrangebylex fooz -x \[bar}
        }

        test "ZREMRANGEBYSCORE basics - $encoding" {
            proc remrangebyscore {min max} {
                create_zset zset {1 a 2 b 3 c 4 d 5 e}
                assert_equal 1 [r exists zset]
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

            # destroy when empty
            assert_equal 5 [remrangebyscore 1 5]
            assert_equal 0 [r exists zset]
        }

        test "ZREMRANGEBYSCORE with non-value min or max - $encoding" {
            assert_error "*not*float*" {r zremrangebyscore fooz str 1}
            assert_error "*not*float*" {r zremrangebyscore fooz 1 str}
            assert_error "*not*float*" {r zremrangebyscore fooz 1 NaN}
        }

        test "ZREMRANGEBYRANK basics - $encoding" {
            proc remrangebyrank {min max} {
                create_zset zset {1 a 2 b 3 c 4 d 5 e}
                assert_equal 1 [r exists zset]
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

            # destroy when empty
            assert_equal 5 [remrangebyrank 0 4]
            assert_equal 0 [r exists zset]
        }

        test "ZUNIONSTORE against non-existing key doesn't set destination - $encoding" {
            r del zseta{t}
            assert_equal 0 [r zunionstore dst_key{t} 1 zseta{t}]
            assert_equal 0 [r exists dst_key{t}]
        }

        test "ZUNION/ZINTER/ZDIFF against non-existing key - $encoding" {
            r del zseta
            assert_equal {} [r zunion 1 zseta]
            assert_equal {} [r zinter 1 zseta]
            assert_equal {} [r zdiff 1 zseta]
        }

        test "ZUNIONSTORE with empty set - $encoding" {
            r del zseta{t} zsetb{t}
            r zadd zseta{t} 1 a
            r zadd zseta{t} 2 b
            r zunionstore zsetc{t} 2 zseta{t} zsetb{t}
            r zrange zsetc{t} 0 -1 withscores
        } {a 1 b 2}

        test "ZUNION/ZINTER/ZDIFF with empty set - $encoding" {
            r del zseta{t} zsetb{t}
            r zadd zseta{t} 1 a
            r zadd zseta{t} 2 b
            assert_equal {a 1 b 2} [r zunion 2 zseta{t} zsetb{t} withscores]
            assert_equal {} [r zinter 2 zseta{t} zsetb{t} withscores]
            assert_equal {a 1 b 2} [r zdiff 2 zseta{t} zsetb{t} withscores]
        }

        test "ZUNIONSTORE basics - $encoding" {
            r del zseta{t} zsetb{t} zsetc{t}
            r zadd zseta{t} 1 a
            r zadd zseta{t} 2 b
            r zadd zseta{t} 3 c
            r zadd zsetb{t} 1 b
            r zadd zsetb{t} 2 c
            r zadd zsetb{t} 3 d

            assert_equal 4 [r zunionstore zsetc{t} 2 zseta{t} zsetb{t}]
            assert_equal {a 1 b 3 d 3 c 5} [r zrange zsetc{t} 0 -1 withscores]
        }

        test "ZUNION/ZINTER/ZDIFF with integer members - $encoding" {
            r del zsetd{t} zsetf{t}
            r zadd zsetd{t} 1 1
            r zadd zsetd{t} 2 2
            r zadd zsetd{t} 3 3
            r zadd zsetf{t} 1 1
            r zadd zsetf{t} 3 3
            r zadd zsetf{t} 4 4

            assert_equal {1 2 2 2 4 4 3 6} [r zunion 2 zsetd{t} zsetf{t} withscores]
            assert_equal {1 2 3 6} [r zinter 2 zsetd{t} zsetf{t} withscores]
            assert_equal {2 2} [r zdiff 2 zsetd{t} zsetf{t} withscores]
        }

        test "ZUNIONSTORE with weights - $encoding" {
            assert_equal 4 [r zunionstore zsetc{t} 2 zseta{t} zsetb{t} weights 2 3]
            assert_equal {a 2 b 7 d 9 c 12} [r zrange zsetc{t} 0 -1 withscores]
        }

        test "ZUNION with weights - $encoding" {
            assert_equal {a 2 b 7 d 9 c 12} [r zunion 2 zseta{t} zsetb{t} weights 2 3 withscores]
            assert_equal {b 7 c 12} [r zinter 2 zseta{t} zsetb{t} weights 2 3 withscores]
        }

        test "ZUNIONSTORE with a regular set and weights - $encoding" {
            r del seta{t}
            r sadd seta{t} a
            r sadd seta{t} b
            r sadd seta{t} c

            assert_equal 4 [r zunionstore zsetc{t} 2 seta{t} zsetb{t} weights 2 3]
            assert_equal {a 2 b 5 c 8 d 9} [r zrange zsetc{t} 0 -1 withscores]
        }

        test "ZUNIONSTORE with AGGREGATE MIN - $encoding" {
            assert_equal 4 [r zunionstore zsetc{t} 2 zseta{t} zsetb{t} aggregate min]
            assert_equal {a 1 b 1 c 2 d 3} [r zrange zsetc{t} 0 -1 withscores]
        }

        test "ZUNION/ZINTER with AGGREGATE MIN - $encoding" {
            assert_equal {a 1 b 1 c 2 d 3} [r zunion 2 zseta{t} zsetb{t} aggregate min withscores]
            assert_equal {b 1 c 2} [r zinter 2 zseta{t} zsetb{t} aggregate min withscores]
        }

        test "ZUNIONSTORE with AGGREGATE MAX - $encoding" {
            assert_equal 4 [r zunionstore zsetc{t} 2 zseta{t} zsetb{t} aggregate max]
            assert_equal {a 1 b 2 c 3 d 3} [r zrange zsetc{t} 0 -1 withscores]
        }

        test "ZUNION/ZINTER with AGGREGATE MAX - $encoding" {
            assert_equal {a 1 b 2 c 3 d 3} [r zunion 2 zseta{t} zsetb{t} aggregate max withscores]
            assert_equal {b 2 c 3} [r zinter 2 zseta{t} zsetb{t} aggregate max withscores]
        }

        test "ZINTERSTORE basics - $encoding" {
            assert_equal 2 [r zinterstore zsetc{t} 2 zseta{t} zsetb{t}]
            assert_equal {b 3 c 5} [r zrange zsetc{t} 0 -1 withscores]
        }

        test "ZINTER basics - $encoding" {
            assert_equal {b 3 c 5} [r zinter 2 zseta{t} zsetb{t} withscores]
        }

        test "ZINTER RESP3 - $encoding" {
            r hello 3
            assert_equal {{b 3.0} {c 5.0}} [r zinter 2 zseta{t} zsetb{t} withscores]
            r hello 2
        }

        test "ZINTERSTORE with weights - $encoding" {
            assert_equal 2 [r zinterstore zsetc{t} 2 zseta{t} zsetb{t} weights 2 3]
            assert_equal {b 7 c 12} [r zrange zsetc{t} 0 -1 withscores]
        }

        test "ZINTER with weights - $encoding" {
            assert_equal {b 7 c 12} [r zinter 2 zseta{t} zsetb{t} weights 2 3 withscores]
        }

        test "ZINTERSTORE with a regular set and weights - $encoding" {
            r del seta{t}
            r sadd seta{t} a
            r sadd seta{t} b
            r sadd seta{t} c
            assert_equal 2 [r zinterstore zsetc{t} 2 seta{t} zsetb{t} weights 2 3]
            assert_equal {b 5 c 8} [r zrange zsetc{t} 0 -1 withscores]
        }

        test "ZINTERSTORE with AGGREGATE MIN - $encoding" {
            assert_equal 2 [r zinterstore zsetc{t} 2 zseta{t} zsetb{t} aggregate min]
            assert_equal {b 1 c 2} [r zrange zsetc{t} 0 -1 withscores]
        }

        test "ZINTERSTORE with AGGREGATE MAX - $encoding" {
            assert_equal 2 [r zinterstore zsetc{t} 2 zseta{t} zsetb{t} aggregate max]
            assert_equal {b 2 c 3} [r zrange zsetc{t} 0 -1 withscores]
        }

        foreach cmd {ZUNIONSTORE ZINTERSTORE} {
            test "$cmd with +inf/-inf scores - $encoding" {
                r del zsetinf1{t} zsetinf2{t}

                r zadd zsetinf1{t} +inf key
                r zadd zsetinf2{t} +inf key
                r $cmd zsetinf3{t} 2 zsetinf1{t} zsetinf2{t}
                assert_equal inf [r zscore zsetinf3{t} key]

                r zadd zsetinf1{t} -inf key
                r zadd zsetinf2{t} +inf key
                r $cmd zsetinf3{t} 2 zsetinf1{t} zsetinf2{t}
                assert_equal 0 [r zscore zsetinf3{t} key]

                r zadd zsetinf1{t} +inf key
                r zadd zsetinf2{t} -inf key
                r $cmd zsetinf3{t} 2 zsetinf1{t} zsetinf2{t}
                assert_equal 0 [r zscore zsetinf3{t} key]

                r zadd zsetinf1{t} -inf key
                r zadd zsetinf2{t} -inf key
                r $cmd zsetinf3{t} 2 zsetinf1{t} zsetinf2{t}
                assert_equal -inf [r zscore zsetinf3{t} key]
            }

            test "$cmd with NaN weights - $encoding" {
                r del zsetinf1{t} zsetinf2{t}

                r zadd zsetinf1{t} 1.0 key
                r zadd zsetinf2{t} 1.0 key
                assert_error "*weight*not*float*" {
                    r $cmd zsetinf3{t} 2 zsetinf1{t} zsetinf2{t} weights nan nan
                }
            }
        }

        test "ZDIFFSTORE basics - $encoding" {
            assert_equal 1 [r zdiffstore zsetc{t} 2 zseta{t} zsetb{t}]
            assert_equal {a 1} [r zrange zsetc{t} 0 -1 withscores]
        }

        test "ZDIFF basics - $encoding" {
            assert_equal {a 1} [r zdiff 2 zseta{t} zsetb{t} withscores]
        }

        test "ZDIFFSTORE with a regular set - $encoding" {
            r del seta{t}
            r sadd seta{t} a
            r sadd seta{t} b
            r sadd seta{t} c
            assert_equal 1 [r zdiffstore zsetc{t} 2 seta{t} zsetb{t}]
            assert_equal {a 1} [r zrange zsetc{t} 0 -1 withscores]
        }

        test "ZDIFF subtracting set from itself - $encoding" {
            assert_equal 0 [r zdiffstore zsetc{t} 2 zseta{t} zseta{t}]
            assert_equal {} [r zrange zsetc{t} 0 -1 withscores]
        }

        test "ZDIFF algorithm 1 - $encoding" {
            r del zseta{t} zsetb{t} zsetc{t}
            r zadd zseta{t} 1 a
            r zadd zseta{t} 2 b
            r zadd zseta{t} 3 c
            r zadd zsetb{t} 1 b
            r zadd zsetb{t} 2 c
            r zadd zsetb{t} 3 d
            assert_equal 1 [r zdiffstore zsetc{t} 2 zseta{t} zsetb{t}]
            assert_equal {a 1} [r zrange zsetc{t} 0 -1 withscores]
        }

        test "ZDIFF algorithm 2 - $encoding" {
            r del zseta{t} zsetb{t} zsetc{t} zsetd{t} zsete{t}
            r zadd zseta{t} 1 a
            r zadd zseta{t} 2 b
            r zadd zseta{t} 3 c
            r zadd zseta{t} 5 e
            r zadd zsetb{t} 1 b
            r zadd zsetc{t} 1 c
            r zadd zsetd{t} 1 d
            assert_equal 2 [r zdiffstore zsete{t} 4 zseta{t} zsetb{t} zsetc{t} zsetd{t}]
            assert_equal {a 1 e 5} [r zrange zsete{t} 0 -1 withscores]
        }

        test "ZDIFF fuzzing - $encoding" {
            for {set j 0} {$j < 100} {incr j} {
                unset -nocomplain s
                array set s {}
                set args {}
                set num_sets [expr {[randomInt 10]+1}]
                for {set i 0} {$i < $num_sets} {incr i} {
                    set num_elements [randomInt 100]
                    r del zset_$i{t}
                    lappend args zset_$i{t}
                    while {$num_elements} {
                        set ele [randomValue]
                        r zadd zset_$i{t} [randomInt 100] $ele
                        if {$i == 0} {
                            set s($ele) x
                        } else {
                            unset -nocomplain s($ele)
                        }
                        incr num_elements -1
                    }
                }
                set result [lsort [r zdiff [llength $args] {*}$args]]
                assert_equal $result [lsort [array names s]]
            }
        }

        test "Basic ZPOP with a single key - $encoding" {
            r del zset
            assert_equal {} [r zpopmin zset]
            create_zset zset {-1 a 1 b 2 c 3 d 4 e}
            assert_equal {a -1} [r zpopmin zset]
            assert_equal {b 1} [r zpopmin zset]
            assert_equal {e 4} [r zpopmax zset]
            assert_equal {d 3} [r zpopmax zset]
            assert_equal {c 2} [r zpopmin zset]
            assert_equal 0 [r exists zset]
            r set foo bar
            assert_error "*WRONGTYPE*" {r zpopmin foo}
        }

        test "ZPOP with count - $encoding" {
            r del z1
            r del z2
            r del z3
            r del foo
            r set foo bar
            assert_equal {} [r zpopmin z1 2]
            assert_error "*WRONGTYPE*" {r zpopmin foo 2}
            create_zset z1 {0 a 1 b 2 c 3 d}
            assert_equal {a 0 b 1} [r zpopmin z1 2]
            assert_equal {d 3 c 2} [r zpopmax z1 2]
        }

        test "BZPOP with a single existing sorted set - $encoding" {
            set rd [redis_deferring_client]
            create_zset zset {0 a 1 b 2 c}

            $rd bzpopmin zset 5
            assert_equal {zset a 0} [$rd read]
            $rd bzpopmin zset 5
            assert_equal {zset b 1} [$rd read]
            $rd bzpopmax zset 5
            assert_equal {zset c 2} [$rd read]
            assert_equal 0 [r exists zset]
        }

        test "BZPOP with multiple existing sorted sets - $encoding" {
            set rd [redis_deferring_client]
            create_zset z1{t} {0 a 1 b 2 c}
            create_zset z2{t} {3 d 4 e 5 f}

            $rd bzpopmin z1{t} z2{t} 5
            assert_equal {z1{t} a 0} [$rd read]
            $rd bzpopmax z1{t} z2{t} 5
            assert_equal {z1{t} c 2} [$rd read]
            assert_equal 1 [r zcard z1{t}]
            assert_equal 3 [r zcard z2{t}]

            $rd bzpopmax z2{t} z1{t} 5
            assert_equal {z2{t} f 5} [$rd read]
            $rd bzpopmin z2{t} z1{t} 5
            assert_equal {z2{t} d 3} [$rd read]
            assert_equal 1 [r zcard z1{t}]
            assert_equal 1 [r zcard z2{t}]
        }

        test "BZPOP second sorted set has members - $encoding" {
            set rd [redis_deferring_client]
            r del z1{t}
            create_zset z2{t} {3 d 4 e 5 f}
            $rd bzpopmax z1{t} z2{t} 5
            assert_equal {z2{t} f 5} [$rd read]
            $rd bzpopmin z2{t} z1{t} 5
            assert_equal {z2{t} d 3} [$rd read]
            assert_equal 0 [r zcard z1{t}]
            assert_equal 1 [r zcard z2{t}]
        }

        test "Basic ZPOP - $encoding RESP3" {
            r hello 3
            r del z1
            create_zset z1 {0 a 1 b 2 c 3 d}
            assert_equal {a 0.0} [r zpopmin z1]
            assert_equal {d 3.0} [r zpopmax z1]
            r hello 2
        }

        test "ZPOP with count - $encoding RESP3" {
            r hello 3
            r del z1
            create_zset z1 {0 a 1 b 2 c 3 d}
            assert_equal {{a 0.0} {b 1.0}} [r zpopmin z1 2]
            assert_equal {{d 3.0} {c 2.0}} [r zpopmax z1 2]
            r hello 2
        }

        test "BZPOP - $encoding RESP3" {
            r hello 3
            set rd [redis_deferring_client]
            create_zset zset {0 a 1 b 2 c}

            $rd bzpopmin zset 5
            assert_equal {zset a 0} [$rd read]
            $rd bzpopmin zset 5
            assert_equal {zset b 1} [$rd read]
            $rd bzpopmax zset 5
            assert_equal {zset c 2} [$rd read]
            assert_equal 0 [r exists zset]
            r hello 2
        }

        r config set zset-max-ziplist-entries $original_max_entries
        r config set zset-max-ziplist-value $original_max_value
    }

    basics ziplist
    basics skiplist

    test {ZINTERSTORE regression with two sets, intset+hashtable} {
        r del seta{t} setb{t} setc{t}
        r sadd set1{t} a
        r sadd set2{t} 10
        r zinterstore set3{t} 2 set1{t} set2{t}
    } {0}

    test {ZUNIONSTORE regression, should not create NaN in scores} {
        r zadd z{t} -inf neginf
        r zunionstore out{t} 1 z{t} weights 0
        r zrange out{t} 0 -1 withscores
    } {neginf 0}

    test {ZINTERSTORE #516 regression, mixed sets and ziplist zsets} {
        r sadd one{t} 100 101 102 103
        r sadd two{t} 100 200 201 202
        r zadd three{t} 1 500 1 501 1 502 1 503 1 100
        r zinterstore to_here{t} 3 one{t} two{t} three{t} WEIGHTS 0 0 1
        r zrange to_here{t} 0 -1
    } {100}

    test {ZUNIONSTORE result is sorted} {
        # Create two sets with common and not common elements, perform
        # the UNION, check that elements are still sorted.
        r del one{t} two{t} dest{t}
        set cmd1 [list r zadd one{t}]
        set cmd2 [list r zadd two{t}]
        for {set j 0} {$j < 1000} {incr j} {
            lappend cmd1 [expr rand()] [randomInt 1000]
            lappend cmd2 [expr rand()] [randomInt 1000]
        }
        {*}$cmd1
        {*}$cmd2
        assert {[r zcard one{t}] > 100}
        assert {[r zcard two{t}] > 100}
        r zunionstore dest{t} 2 one{t} two{t}
        set oldscore 0
        foreach {ele score} [r zrange dest{t} 0 -1 withscores] {
            assert {$score >= $oldscore}
            set oldscore $score
        }
    }

    test "ZUNIONSTORE/ZINTERSTORE/ZDIFFSTORE error if using WITHSCORES " {
        assert_error "*ERR*syntax*" {r zunionstore foo{t} 2 zsetd{t} zsetf{t} withscores}
        assert_error "*ERR*syntax*" {r zinterstore foo{t} 2 zsetd{t} zsetf{t} withscores}
        assert_error "*ERR*syntax*" {r zdiffstore foo{t} 2 zsetd{t} zsetf{t} withscores}
    }
    
    test {ZMSCORE retrieve} {
        r del zmscoretest
        r zadd zmscoretest 10 x
        r zadd zmscoretest 20 y
        
        r zmscore zmscoretest x y
    } {10 20}

    test {ZMSCORE retrieve from empty set} {
        r del zmscoretest
        
        r zmscore zmscoretest x y
    } {{} {}}
    
    test {ZMSCORE retrieve with missing member} {
        r del zmscoretest
        r zadd zmscoretest 10 x
        
        r zmscore zmscoretest x y
    } {10 {}}

    test {ZMSCORE retrieve single member} {
        r del zmscoretest
        r zadd zmscoretest 10 x
        r zadd zmscoretest 20 y
        
        r zmscore zmscoretest x
    } {10}

    test {ZMSCORE retrieve requires one or more members} {
        r del zmscoretest
        r zadd zmscoretest 10 x
        r zadd zmscoretest 20 y
        
        catch {r zmscore zmscoretest} e
        assert_match {*ERR*wrong*number*arg*} $e
    }

    test "ZSET commands don't accept the empty strings as valid score" {
        assert_error "*not*float*" {r zadd myzset "" abc}
    }

    proc stressers {encoding} {
        set original_max_entries [lindex [r config get zset-max-ziplist-entries] 1]
        set original_max_value [lindex [r config get zset-max-ziplist-value] 1]
        if {$encoding == "ziplist"} {
            # Little extra to allow proper fuzzing in the sorting stresser
            r config set zset-max-ziplist-entries 256
            r config set zset-max-ziplist-value 64
            set elements 128
        } elseif {$encoding == "skiplist"} {
            r config set zset-max-ziplist-entries 0
            r config set zset-max-ziplist-value 0
            if {$::accurate} {set elements 1000} else {set elements 100}
        } else {
            puts "Unknown sorted set encoding"
            exit
        }

        test "ZSCORE - $encoding" {
            r del zscoretest
            set aux {}
            for {set i 0} {$i < $elements} {incr i} {
                set score [expr rand()]
                lappend aux $score
                r zadd zscoretest $score $i
            }

            assert_encoding $encoding zscoretest
            for {set i 0} {$i < $elements} {incr i} {
                assert_equal [lindex $aux $i] [r zscore zscoretest $i]
            }
        }

        test "ZMSCORE - $encoding" {
            r del zscoretest
            set aux {}
            for {set i 0} {$i < $elements} {incr i} {
                set score [expr rand()]
                lappend aux $score
                r zadd zscoretest $score $i
            }

            assert_encoding $encoding zscoretest
            for {set i 0} {$i < $elements} {incr i} {
                assert_equal [lindex $aux $i] [r zmscore zscoretest $i]
            }
        }

        test "ZSCORE after a DEBUG RELOAD - $encoding" {
            r del zscoretest
            set aux {}
            for {set i 0} {$i < $elements} {incr i} {
                set score [expr rand()]
                lappend aux $score
                r zadd zscoretest $score $i
            }

            r debug reload
            assert_encoding $encoding zscoretest
            for {set i 0} {$i < $elements} {incr i} {
                assert_equal [lindex $aux $i] [r zscore zscoretest $i]
            }
        } {} {needs:debug}

        test "ZSET sorting stresser - $encoding" {
            set delta 0
            for {set test 0} {$test < 2} {incr test} {
                unset -nocomplain auxarray
                array set auxarray {}
                set auxlist {}
                r del myzset
                for {set i 0} {$i < $elements} {incr i} {
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

                assert_encoding $encoding myzset
                set fromredis [r zrange myzset 0 -1]
                set delta 0
                for {set i 0} {$i < [llength $fromredis]} {incr i} {
                    if {[lindex $fromredis $i] != [lindex $auxlist $i]} {
                        incr delta
                    }
                }
            }
            assert_equal 0 $delta
        }

        test "ZRANGEBYSCORE fuzzy test, 100 ranges in $elements element sorted set - $encoding" {
            set err {}
            r del zset
            for {set i 0} {$i < $elements} {incr i} {
                r zadd zset [expr rand()] $i
            }

            assert_encoding $encoding zset
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
            assert_equal {} $err
        }

        test "ZRANGEBYLEX fuzzy test, 100 ranges in $elements element sorted set - $encoding" {
            set lexset {}
            r del zset
            for {set j 0} {$j < $elements} {incr j} {
                set e [randstring 0 30 alpha]
                lappend lexset $e
                r zadd zset 0 $e
            }
            set lexset [lsort -unique $lexset]
            for {set j 0} {$j < 100} {incr j} {
                set min [randstring 0 30 alpha]
                set max [randstring 0 30 alpha]
                set mininc [randomInt 2]
                set maxinc [randomInt 2]
                if {$mininc} {set cmin "\[$min"} else {set cmin "($min"}
                if {$maxinc} {set cmax "\[$max"} else {set cmax "($max"}
                set rev [randomInt 2]
                if {$rev} {
                    set cmd zrevrangebylex
                } else {
                    set cmd zrangebylex
                }

                # Make sure data is the same in both sides
                assert {[r zrange zset 0 -1] eq $lexset}

                # Get the Redis output
                set output [r $cmd zset $cmin $cmax]
                if {$rev} {
                    set outlen [r zlexcount zset $cmax $cmin]
                } else {
                    set outlen [r zlexcount zset $cmin $cmax]
                }

                # Compute the same output via Tcl
                set o {}
                set copy $lexset
                if {(!$rev && [string compare $min $max] > 0) ||
                    ($rev && [string compare $max $min] > 0)} {
                    # Empty output when ranges are inverted.
                } else {
                    if {$rev} {
                        # Invert the Tcl array using Redis itself.
                        set copy [r zrevrange zset 0 -1]
                        # Invert min / max as well
                        lassign [list $min $max $mininc $maxinc] \
                            max min maxinc mininc
                    }
                    foreach e $copy {
                        set mincmp [string compare $e $min]
                        set maxcmp [string compare $e $max]
                        if {
                             ($mininc && $mincmp >= 0 || !$mininc && $mincmp > 0)
                             &&
                             ($maxinc && $maxcmp <= 0 || !$maxinc && $maxcmp < 0)
                        } {
                            lappend o $e
                        }
                    }
                }
                assert {$o eq $output}
                assert {$outlen eq [llength $output]}
            }
        }

        test "ZREMRANGEBYLEX fuzzy test, 100 ranges in $elements element sorted set - $encoding" {
            set lexset {}
            r del zset{t} zsetcopy{t}
            for {set j 0} {$j < $elements} {incr j} {
                set e [randstring 0 30 alpha]
                lappend lexset $e
                r zadd zset{t} 0 $e
            }
            set lexset [lsort -unique $lexset]
            for {set j 0} {$j < 100} {incr j} {
                # Copy...
                r zunionstore zsetcopy{t} 1 zset{t}
                set lexsetcopy $lexset

                set min [randstring 0 30 alpha]
                set max [randstring 0 30 alpha]
                set mininc [randomInt 2]
                set maxinc [randomInt 2]
                if {$mininc} {set cmin "\[$min"} else {set cmin "($min"}
                if {$maxinc} {set cmax "\[$max"} else {set cmax "($max"}

                # Make sure data is the same in both sides
                assert {[r zrange zset{t} 0 -1] eq $lexset}

                # Get the range we are going to remove
                set torem [r zrangebylex zset{t} $cmin $cmax]
                set toremlen [r zlexcount zset{t} $cmin $cmax]
                r zremrangebylex zsetcopy{t} $cmin $cmax
                set output [r zrange zsetcopy{t} 0 -1]

                # Remove the range with Tcl from the original list
                if {$toremlen} {
                    set first [lsearch -exact $lexsetcopy [lindex $torem 0]]
                    set last [expr {$first+$toremlen-1}]
                    set lexsetcopy [lreplace $lexsetcopy $first $last]
                }
                assert {$lexsetcopy eq $output}
            }
        }

        test "ZSETs skiplist implementation backlink consistency test - $encoding" {
            set diff 0
            for {set j 0} {$j < $elements} {incr j} {
                r zadd myzset [expr rand()] "Element-$j"
                r zrem myzset "Element-[expr int(rand()*$elements)]"
            }

            assert_encoding $encoding myzset
            set l1 [r zrange myzset 0 -1]
            set l2 [r zrevrange myzset 0 -1]
            for {set j 0} {$j < [llength $l1]} {incr j} {
                if {[lindex $l1 $j] ne [lindex $l2 end-$j]} {
                    incr diff
                }
            }
            assert_equal 0 $diff
        }

        test "ZSETs ZRANK augmented skip list stress testing - $encoding" {
            set err {}
            r del myzset
            for {set k 0} {$k < 2000} {incr k} {
                set i [expr {$k % $elements}]
                if {[expr rand()] < .2} {
                    r zrem myzset $i
                } else {
                    set score [expr rand()]
                    r zadd myzset $score $i
                    assert_encoding $encoding myzset
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
            assert_equal {} $err
        }

        test "BZPOPMIN, ZADD + DEL should not awake blocked client" {
            set rd [redis_deferring_client]
            r del zset

            $rd bzpopmin zset 0
            r multi
            r zadd zset 0 foo
            r del zset
            r exec
            r del zset
            r zadd zset 1 bar
            $rd read
        } {zset bar 1}

        test "BZPOPMIN, ZADD + DEL + SET should not awake blocked client" {
            set rd [redis_deferring_client]
            r del list

            r del zset

            $rd bzpopmin zset 0
            r multi
            r zadd zset 0 foo
            r del zset
            r set zset foo
            r exec
            r del zset
            r zadd zset 1 bar
            $rd read
        } {zset bar 1}

        test "BZPOPMIN with same key multiple times should work" {
            set rd [redis_deferring_client]
            r del z1{t} z2{t}

            # Data arriving after the BZPOPMIN.
            $rd bzpopmin z1{t} z2{t} z2{t} z1{t} 0
            r zadd z1{t} 0 a
            assert_equal [$rd read] {z1{t} a 0}
            $rd bzpopmin z1{t} z2{t} z2{t} z1{t} 0
            r zadd z2{t} 1 b
            assert_equal [$rd read] {z2{t} b 1}

            # Data already there.
            r zadd z1{t} 0 a
            r zadd z2{t} 1 b
            $rd bzpopmin z1{t} z2{t} z2{t} z1{t} 0
            assert_equal [$rd read] {z1{t} a 0}
            $rd bzpopmin z1{t} z2{t} z2{t} z1{t} 0
            assert_equal [$rd read] {z2{t} b 1}
        }

        test "MULTI/EXEC is isolated from the point of view of BZPOPMIN" {
            set rd [redis_deferring_client]
            r del zset
            $rd bzpopmin zset 0
            r multi
            r zadd zset 0 a
            r zadd zset 1 b
            r zadd zset 2 c
            r exec
            $rd read
        } {zset a 0}

        test "BZPOPMIN with variadic ZADD" {
            set rd [redis_deferring_client]
            r del zset
            if {$::valgrind} {after 100}
            $rd bzpopmin zset 0
            if {$::valgrind} {after 100}
            assert_equal 2 [r zadd zset -1 foo 1 bar]
            if {$::valgrind} {after 100}
            assert_equal {zset foo -1} [$rd read]
            assert_equal {bar} [r zrange zset 0 -1]
        }

        test "BZPOPMIN with zero timeout should block indefinitely" {
            set rd [redis_deferring_client]
            r del zset
            $rd bzpopmin zset 0
            after 1000
            r zadd zset 0 foo
            assert_equal {zset foo 0} [$rd read]
        }
        r config set zset-max-ziplist-entries $original_max_entries
        r config set zset-max-ziplist-value $original_max_value
    }

    tags {"slow"} {
        stressers ziplist
        stressers skiplist
    }

    test {ZSET skiplist order consistency when elements are moved} {
        set original_max [lindex [r config get zset-max-ziplist-entries] 1]
        r config set zset-max-ziplist-entries 0
        for {set times 0} {$times < 10} {incr times} {
            r del zset
            for {set j 0} {$j < 1000} {incr j} {
                r zadd zset [randomInt 50] ele-[randomInt 10]
            }

            # Make sure that element ordering is correct
            set prev_element {}
            set prev_score -1
            foreach {element score} [r zrange zset 0 -1 WITHSCORES] {
                # Assert that elements are in increasing ordering
                assert {
                    $prev_score < $score ||
                    ($prev_score == $score &&
                     [string compare $prev_element $element] == -1)
                }
                set prev_element $element
                set prev_score $score
            }
        }
        r config set zset-max-ziplist-entries $original_max
    }

    test {ZRANGESTORE basic} {
        r flushall
        r zadd z1{t} 1 a 2 b 3 c 4 d
        set res [r zrangestore z2{t} z1{t} 0 -1]
        assert_equal $res 4
        r zrange z2{t} 0 -1 withscores
    } {a 1 b 2 c 3 d 4}

    test {ZRANGESTORE RESP3} {
        r hello 3
        assert_equal [r zrange z2{t} 0 -1 withscores] {{a 1.0} {b 2.0} {c 3.0} {d 4.0}}
        r hello 2
    } 

    test {ZRANGESTORE range} {
        set res [r zrangestore z2{t} z1{t} 1 2]
        assert_equal $res 2
        r zrange z2{t} 0 -1 withscores
    } {b 2 c 3}

    test {ZRANGESTORE BYLEX} {
        set res [r zrangestore z2{t} z1{t} \[b \[c BYLEX]
        assert_equal $res 2
        r zrange z2{t} 0 -1 withscores
    } {b 2 c 3}

    test {ZRANGESTORE BYSCORE} {
        set res [r zrangestore z2{t} z1{t} 1 2 BYSCORE]
        assert_equal $res 2
        r zrange z2{t} 0 -1 withscores
    } {a 1 b 2}

    test {ZRANGESTORE BYSCORE LIMIT} {
        set res [r zrangestore z2{t} z1{t} 0 5 BYSCORE LIMIT 0 2]
        assert_equal $res 2
        r zrange z2{t} 0 -1 withscores
    } {a 1 b 2}

    test {ZRANGESTORE BYSCORE REV LIMIT} {
        set res [r zrangestore z2{t} z1{t} 5 0 BYSCORE REV LIMIT 0 2]
        assert_equal $res 2
        r zrange z2{t} 0 -1 withscores
    } {c 3 d 4}

    test {ZRANGE BYSCORE REV LIMIT} {
        r zrange z1{t} 5 0 BYSCORE REV LIMIT 0 2 WITHSCORES
    } {d 4 c 3}

    test {ZRANGESTORE - src key missing} {
        set res [r zrangestore z2{t} missing{t} 0 -1]
        assert_equal $res 0
        r exists z2{t}
    } {0}

    test {ZRANGESTORE - src key wrong type} {
        r zadd z2{t} 1 a
        r set foo{t} bar
        assert_error "*WRONGTYPE*" {r zrangestore z2{t} foo{t} 0 -1}
        r zrange z2{t} 0 -1
    } {a}

    test {ZRANGESTORE - empty range} {
        set res [r zrangestore z2{t} z1{t} 5 6]
        assert_equal $res 0
        r exists z2{t}
    } {0}

    test {ZRANGESTORE BYLEX - empty range} {
        set res [r zrangestore z2{t} z1{t} \[f \[g BYLEX]
        assert_equal $res 0
        r exists z2{t}
    } {0}

    test {ZRANGESTORE BYSCORE - empty range} {
        set res [r zrangestore z2{t} z1{t} 5 6 BYSCORE]
        assert_equal $res 0
        r exists z2{t}
    } {0}

    test {ZRANGE BYLEX} {
        r zrange z1{t} \[b \[c BYLEX
    } {b c}

    test {ZRANGESTORE invalid syntax} {
        catch {r zrangestore z2{t} z1{t} 0 -1 limit 1 2} err
        assert_match "*syntax*" $err
        catch {r zrangestore z2{t} z1{t} 0 -1 WITHSCORES} err
        assert_match "*syntax*" $err
    }

    test {ZRANGE invalid syntax} {
        catch {r zrange z1{t} 0 -1 limit 1 2} err
        assert_match "*syntax*" $err
        catch {r zrange z1{t} 0 -1 BYLEX WITHSCORES} err
        assert_match "*syntax*" $err
        catch {r zrevrange z1{t} 0 -1 BYSCORE} err
        assert_match "*syntax*" $err
        catch {r zrangebyscore z1{t} 0 -1 REV} err
        assert_match "*syntax*" $err
    }

    proc get_keys {l} {
        set res {}
        foreach {score key} $l {
            lappend res $key
        }
        return $res
    }

    # Check whether the zset members belong to the zset
    proc check_member {mydict res} {
        foreach ele $res {
            assert {[dict exists $mydict $ele]}
        }
    }

    # Check whether the zset members and score belong to the zset
    proc check_member_and_score {mydict res} {
       foreach {key val} $res {
            assert_equal $val [dict get $mydict $key]
        }
    }

    foreach {type contents} "ziplist {1 a 2 b 3 c} skiplist {1 a 2 b 3 [randstring 70 90 alpha]}" {
        set original_max_value [lindex [r config get zset-max-ziplist-value] 1]
        r config set zset-max-ziplist-value 10
        create_zset myzset $contents
        assert_encoding $type myzset

        test "ZRANDMEMBER - $type" {
            unset -nocomplain myzset
            array set myzset {}
            for {set i 0} {$i < 100} {incr i} {
                set key [r zrandmember myzset]
                set myzset($key) 1
            }
            assert_equal [lsort [get_keys $contents]] [lsort [array names myzset]]
        }
        r config set zset-max-ziplist-value $original_max_value
    }

    test "ZRANDMEMBER with RESP3" {
        r hello 3
        set res [r zrandmember myzset 3 withscores]
        assert_equal [llength $res] 3
        assert_equal [llength [lindex $res 1]] 2

        set res [r zrandmember myzset 3]
        assert_equal [llength $res] 3
        assert_equal [llength [lindex $res 1]] 1
        r hello 2
    }

    test "ZRANDMEMBER count of 0 is handled correctly" {
        r zrandmember myzset 0
    } {}

    test "ZRANDMEMBER with <count> against non existing key" {
        r zrandmember nonexisting_key 100
    } {}

    foreach {type contents} "
        skiplist {1 a 2 b 3 c 4 d 5 e 6 f 7 g 7 h 9 i 10 [randstring 70 90 alpha]}
        ziplist {1 a 2 b 3 c 4 d 5 e 6 f 7 g 7 h 9 i 10 j} " {
        test "ZRANDMEMBER with <count> - $type" {
            set original_max_value [lindex [r config get zset-max-ziplist-value] 1]
            r config set zset-max-ziplist-value 10
            create_zset myzset $contents
            assert_encoding $type myzset

            # create a dict for easy lookup
            unset -nocomplain mydict
            foreach {k v} [r zrange myzset 0 -1 withscores] {
                dict append mydict $k $v
            }

            # We'll stress different parts of the code, see the implementation
            # of ZRANDMEMBER for more information, but basically there are
            # four different code paths.

            # PATH 1: Use negative count.

            # 1) Check that it returns repeated elements with and without values.
            # 2) Check that all the elements actually belong to the original zset.
            set res [r zrandmember myzset -20]
            assert_equal [llength $res] 20
            check_member $mydict $res

            set res [r zrandmember myzset -1001]
            assert_equal [llength $res] 1001
            check_member $mydict $res

            # again with WITHSCORES
            set res [r zrandmember myzset -20 withscores]
            assert_equal [llength $res] 40
            check_member_and_score $mydict $res

            set res [r zrandmember myzset -1001 withscores]
            assert_equal [llength $res] 2002
            check_member_and_score $mydict $res

            # Test random uniform distribution
            # df = 9, 40 means 0.00001 probability
            set res [r zrandmember myzset -1000]
            assert_lessthan [chi_square_value $res] 40
            check_member $mydict $res

            # 3) Check that eventually all the elements are returned.
            #    Use both WITHSCORES and without
            unset -nocomplain auxset
            set iterations 1000
            while {$iterations != 0} {
                incr iterations -1
                if {[expr {$iterations % 2}] == 0} {
                    set res [r zrandmember myzset -3 withscores]
                    foreach {key val} $res {
                        dict append auxset $key $val
                    }
                } else {
                    set res [r zrandmember myzset -3]
                    foreach key $res {
                        dict append auxset $key
                    }
                }
                if {[lsort [dict keys $mydict]] eq
                    [lsort [dict keys $auxset]]} {
                    break;
                }
            }
            assert {$iterations != 0}

            # PATH 2: positive count (unique behavior) with requested size
            # equal or greater than set size.
            foreach size {10 20} {
                set res [r zrandmember myzset $size]
                assert_equal [llength $res] 10
                assert_equal [lsort $res] [lsort [dict keys $mydict]]
                check_member $mydict $res

                # again with WITHSCORES
                set res [r zrandmember myzset $size withscores]
                assert_equal [llength $res] 20
                assert_equal [lsort $res] [lsort $mydict]
                check_member_and_score $mydict $res
            }

            # PATH 3: Ask almost as elements as there are in the set.
            # In this case the implementation will duplicate the original
            # set and will remove random elements up to the requested size.
            #
            # PATH 4: Ask a number of elements definitely smaller than
            # the set size.
            #
            # We can test both the code paths just changing the size but
            # using the same code.
            foreach size {1 2 8} {
                # 1) Check that all the elements actually belong to the
                # original set.
                set res [r zrandmember myzset $size]
                assert_equal [llength $res] $size
                check_member $mydict $res

                # again with WITHSCORES
                set res [r zrandmember myzset $size withscores]
                assert_equal [llength $res] [expr {$size * 2}]
                check_member_and_score $mydict $res

                # 2) Check that eventually all the elements are returned.
                #    Use both WITHSCORES and without
                unset -nocomplain auxset
                unset -nocomplain allkey
                set iterations [expr {1000 / $size}]
                set all_ele_return false
                while {$iterations != 0} {
                    incr iterations -1
                    if {[expr {$iterations % 2}] == 0} {
                        set res [r zrandmember myzset $size withscores]
                        foreach {key value} $res {
                            dict append auxset $key $value
                            lappend allkey $key
                        }
                    } else {
                        set res [r zrandmember myzset $size]
                        foreach key $res {
                            dict append auxset $key
                            lappend allkey $key
                        }
                    }
                    if {[lsort [dict keys $mydict]] eq
                        [lsort [dict keys $auxset]]} {
                        set all_ele_return true
                    }
                }
                assert_equal $all_ele_return true
                # df = 9, 40 means 0.00001 probability
                assert_lessthan [chi_square_value $allkey] 40
            }
        }
        r config set zset-max-ziplist-value $original_max_value
    }

}
