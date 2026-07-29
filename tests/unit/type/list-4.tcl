start_server {
    tags {"list"}
    overrides {
        "list-max-ziplist-size" -1
    }
} {
    array set largevalue [generate_largevalue_test_array]

    proc create_listpack {key entries} {
        r del $key
        foreach entry $entries { r rpush $key $entry }
        assert_encoding listpack $key
    }

    proc create_quicklist {key entries} {
        r del $key
        foreach entry $entries { r rpush $key $entry }
        assert_encoding quicklist $key
    }

foreach {type large} [array get largevalue] {
    test "LMOVEM single element, array reply (like LMOVE) - $type" {
        r del src{t} dst{t}
        create_$type src{t} "a b c $large"
        assert_equal {a} [r lmovem src{t} dst{t} left right]
        assert_equal {a} [r lrange dst{t} 0 -1]
        assert_equal "b c $large" [r lrange src{t} 0 -1]
    }

    test "LMOVEM COUNT left pop, OBO vs BULK ordering - $type" {
        r del src{t} dst{t}
        create_$type src{t} "1 2 3 4 5 $large"
        # OBO: each element pushed as popped -> block order reversed at head.
        assert_equal {3 2 1} [r lmovem src{t} dst{t} left left count 3 obo]
        assert_equal {3 2 1} [r lrange dst{t} 0 -1]
        assert_equal "4 5 $large" [r lrange src{t} 0 -1]

        r del src{t} dst{t}
        create_$type src{t} "1 2 3 4 5 $large"
        # BULK: source relative order preserved at head.
        assert_equal {1 2 3} [r lmovem src{t} dst{t} left left count 3 bulk]
        assert_equal {1 2 3} [r lrange dst{t} 0 -1]
        assert_equal "4 5 $large" [r lrange src{t} 0 -1]
    }

    test "LMOVEM COUNT right pop, OBO vs BULK ordering - $type" {
        r del src{t} dst{t}
        create_$type src{t} "$large 1 2 3 4 5"
        # pop order from the tail is 5 4 3.
        assert_equal {5 4 3} [r lmovem src{t} dst{t} right right count 3 obo]
        assert_equal {5 4 3} [r lrange dst{t} 0 -1]
        assert_equal "$large 1 2" [r lrange src{t} 0 -1]

        r del src{t} dst{t}
        create_$type src{t} "$large 1 2 3 4 5"
        assert_equal {3 4 5} [r lmovem src{t} dst{t} right right count 3 bulk]
        assert_equal {3 4 5} [r lrange dst{t} 0 -1]
    }

    test "LMOVEM COUNT moves fewer when source is shorter - $type" {
        r del src{t} dst{t}
        create_$type src{t} "a b $large"
        assert_equal "a b $large" [r lmovem src{t} dst{t} left right count 100 bulk]
        assert_equal 0 [r exists src{t}]
        assert_equal "a b $large" [r lrange dst{t} 0 -1]
    }

    test "LMOVEM EXACTLY success - $type" {
        r del src{t} dst{t}
        create_$type src{t} "1 2 3 $large"
        assert_equal {1 2 3} [r lmovem src{t} dst{t} left right exactly 3 bulk]
        assert_equal {1 2 3} [r lrange dst{t} 0 -1]
        assert_equal "$large" [r lrange src{t} 0 -1]
    }

    test "LMOVEM EXACTLY too few moves nothing, replies nil - $type" {
        r del src{t} dst{t}
        create_$type src{t} "a b $large"
        assert_equal {} [r lmovem src{t} dst{t} left right exactly 4 bulk]
        # Source unchanged, destination not created.
        assert_equal "a b $large" [r lrange src{t} 0 -1]
        assert_equal 0 [r exists dst{t}]
    }

    test "LMOVEM into existing destination - $type" {
        r del src{t} dst{t}
        create_$type src{t} "1 2 3 $large"
        create_$type dst{t} "x y $large"
        assert_equal {1 2} [r lmovem src{t} dst{t} left right count 2 bulk]
        assert_equal "x y $large 1 2" [r lrange dst{t} 0 -1]
    }

    test "LMOVEM same source and destination rotates - $type" {
        r del k{t}
        create_$type k{t} "1 2 3 4 $large"
        assert_equal {1 2} [r lmovem k{t} k{t} left right count 2 bulk]
        assert_equal "3 4 $large 1 2" [r lrange k{t} 0 -1]
    }
}

    test {LMOVEM missing or empty source} {
        r del src{t} dst{t}
        assert_equal {} [r lmovem src{t} dst{t} left right]
        assert_equal {} [r lmovem src{t} dst{t} left right count 5 bulk]
        assert_equal 0 [r exists dst{t}]
    }

    test {LMOVEM count must be positive} {
        r del src{t} dst{t}
        r rpush src{t} a b c
        assert_error "ERR count*" {r lmovem src{t} dst{t} left right count 0 bulk}
        assert_error "ERR count*" {r lmovem src{t} dst{t} left right exactly 0 bulk}
        assert_error "ERR count*" {r lmovem src{t} dst{t} left right count -1 bulk}
        assert_error "ERR count*" {r lmovem src{t} dst{t} left right count a bulk}
    }

    test {LMOVEM syntax errors} {
        r del src{t} dst{t}
        r rpush src{t} a b c
        assert_error "ERR wrong number of arguments for 'lmovem' command" {r lmovem src{t} dst{t} left}
        # OBO|BULK is required when COUNT/EXACTLY is given.
        assert_error "ERR syntax error*" {r lmovem src{t} dst{t} left right count 2}
        assert_error "ERR syntax error*" {r lmovem src{t} dst{t} left right count 2 obo extra}
        assert_error "ERR syntax error*" {r lmovem src{t} dst{t} left right bad 2 obo}
        assert_error "ERR syntax error*" {r lmovem src{t} dst{t} left right count 2 badorder}
        assert_error "ERR syntax error*" {r lmovem src{t} dst{t} badwhere right count 2 obo}
        assert_error "ERR syntax error*" {r lmovem src{t} dst{t} left badwhere count 2 obo}
    }

    test {LMOVEM wrong type} {
        r del s{t} d{t}
        r set s{t} foo
        assert_error "WRONGTYPE*" {r lmovem s{t} d{t} left right}

        r del s{t} d{t}
        r rpush s{t} a b
        r set d{t} foo
        assert_error "WRONGTYPE*" {r lmovem s{t} d{t} left right count 2 bulk}
        # Nothing moved out of the source on a destination type error.
        assert_equal {a b} [r lrange s{t} 0 -1]
    }

    test {LMOVEM propagates verbatim to replica} {
        r del src{t} dst{t}
        r rpush src{t} 1 2 3 4
        set repl [attach_to_replication_stream]
        r lmovem src{t} dst{t} left right count 2 bulk
        r lmovem src{t} dst{t} left right exactly 1 obo
        assert_replication_stream $repl {
            {select *}
            {lmovem src{t} dst{t} left right count 2 bulk}
            {lmovem src{t} dst{t} left right exactly 1 obo}
        }
        close_replication_stream $repl
    } {} {needs:repl}

    test {BLMOVEM with elements available behaves like LMOVEM} {
        r del src{t} dst{t}
        r rpush src{t} 1 2 3
        assert_equal {1 2} [r blmovem src{t} dst{t} left right 0 count 2 bulk]
        assert_equal {1 2} [r lrange dst{t} 0 -1]
    }

    test {BLMOVEM propagates as LMOVEM EXACTLY to replica} {
        r del src{t} dst{t}
        r rpush src{t} 1 2 3 4
        set repl [attach_to_replication_stream]
        r blmovem src{t} dst{t} left right 0 count 2 bulk
        assert_replication_stream $repl {
            {select *}
            {lmovem src{t} dst{t} left right EXACTLY 2 BULK}
        }
        close_replication_stream $repl
    } {} {needs:repl}

    test {BLMOVEM COUNT unblocks on first push} {
        r del src{t} dst{t}
        set rd [redis_deferring_client]
        $rd blmovem src{t} dst{t} left right 0 count 5 bulk
        wait_for_blocked_client
        r rpush src{t} a b
        assert_equal {a b} [$rd read]
        assert_equal {a b} [r lrange dst{t} 0 -1]
        $rd close
    }

    test {BLMOVEM EXACTLY blocks until enough elements then moves atomically} {
        r del src{t} dst{t}
        set rd [redis_deferring_client]
        $rd blmovem src{t} dst{t} left right 0 exactly 3 bulk
        wait_for_blocked_client

        # Not enough yet: the client must stay blocked and nothing is moved.
        r rpush src{t} 1 2
        wait_for_blocked_client
        assert_equal {1 2} [r lrange src{t} 0 -1]
        assert_equal 0 [r exists dst{t}]

        # Now there are enough elements: move exactly 3.
        r rpush src{t} 3
        assert_equal {1 2 3} [$rd read]
        assert_equal {1 2 3} [r lrange dst{t} 0 -1]
        assert_equal 0 [r exists src{t}]
        $rd close
    }

    test {BLMOVEM EXACTLY is woken by LINSERT growing the source} {
        r del src{t} dst{t}
        r rpush src{t} 1 2
        set rd [redis_deferring_client]
        $rd blmovem src{t} dst{t} left right 0 exactly 3 bulk
        wait_for_blocked_client
        r linsert src{t} before 1 0   ;# src -> 0 1 2, reaches 3
        assert_equal {0 1 2} [$rd read]
        assert_equal {0 1 2} [r lrange dst{t} 0 -1]
        assert_equal 0 [r exists src{t}]
        $rd close
    }

    test {BLMOVEM EXACTLY is woken by LMOVE into the source} {
        r del src{t} dst{t} feed{t}
        r rpush src{t} 1 2
        r rpush feed{t} 9
        set rd [redis_deferring_client]
        $rd blmovem src{t} dst{t} left right 0 exactly 3 bulk
        wait_for_blocked_client
        r lmove feed{t} src{t} left right   ;# src -> 1 2 9, reaches 3
        assert_equal {1 2 9} [$rd read]
        assert_equal {1 2 9} [r lrange dst{t} 0 -1]
        assert_equal 0 [r exists src{t}]
        $rd close
    }

    test {BLMOVEM EXACTLY is woken by SORT STORE overwriting the source} {
        r del src{t} dst{t} feed{t}
        r rpush src{t} 1
        r rpush feed{t} 3 1 2
        set rd [redis_deferring_client]
        $rd blmovem src{t} dst{t} left right 0 exactly 3 bulk
        wait_for_blocked_client
        r sort feed{t} store src{t}   ;# src overwritten -> 1 2 3, reaches 3
        assert_equal {1 2 3} [$rd read]
        assert_equal {1 2 3} [r lrange dst{t} 0 -1]
        assert_equal 0 [r exists src{t}]
        $rd close
    }

    test {BLMOVEM times out with null array} {
        r del src{t} dst{t}
        set rd [redis_deferring_client]
        $rd blmovem src{t} dst{t} left right 1 exactly 3 bulk
        wait_for_blocked_client
        assert_equal {} [$rd read]
        $rd close
    }

    test {BLMOVEM inside MULTI does not block} {
        r del src{t} dst{t}
        r multi
        r blmovem src{t} dst{t} left right 0 exactly 3 bulk
        assert_equal {{}} [r exec]
    }
}
