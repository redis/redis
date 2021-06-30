start_server {
    tags {"sort"}
    overrides {
        "list-max-ziplist-size" 32
        "set-max-intset-entries" 32
    }
} {
    proc create_random_dataset {num cmd} {
        set tosort {}
        set result {}
        array set seenrand {}
        r del tosort
        for {set i 0} {$i < $num} {incr i} {
            # Make sure all the weights are different because
            # Redis does not use a stable sort but Tcl does.
            while 1 {
                randpath {
                    set rint [expr int(rand()*1000000)]
                } {
                    set rint [expr rand()]
                }
                if {![info exists seenrand($rint)]} break
            }
            set seenrand($rint) x
            r $cmd tosort $i
            r set weight_$i $rint
            r hset wobj_$i weight $rint
            lappend tosort [list $i $rint]
        }
        set sorted [lsort -index 1 -real $tosort]
        for {set i 0} {$i < $num} {incr i} {
            lappend result [lindex $sorted $i 0]
        }
        set _ $result
    }

    foreach {num cmd enc title} {
        16 lpush quicklist "Old Ziplist"
        1000 lpush quicklist "Old Linked list"
        10000 lpush quicklist "Old Big Linked list"
        16 sadd intset "Intset"
        1000 sadd hashtable "Hash table"
        10000 sadd hashtable "Big Hash table"
    } {
        set result [create_random_dataset $num $cmd]
        assert_encoding $enc tosort

        test "$title: SORT BY key" {
            assert_equal $result [r sort tosort BY weight_*]
        } {} {cluster:skip}

        test "$title: SORT BY key with limit" {
            assert_equal [lrange $result 5 9] [r sort tosort BY weight_* LIMIT 5 5]
        } {} {cluster:skip}

        test "$title: SORT BY hash field" {
            assert_equal $result [r sort tosort BY wobj_*->weight]
        } {} {cluster:skip}
    }

    set result [create_random_dataset 16 lpush]
    test "SORT GET #" {
        assert_equal [lsort -integer $result] [r sort tosort GET #]
    } {} {cluster:skip}

    test "SORT GET <const>" {
        r del foo
        set res [r sort tosort GET foo]
        assert_equal 16 [llength $res]
        foreach item $res { assert_equal {} $item }
    } {} {cluster:skip}

    test "SORT GET (key and hash) with sanity check" {
        set l1 [r sort tosort GET # GET weight_*]
        set l2 [r sort tosort GET # GET wobj_*->weight]
        foreach {id1 w1} $l1 {id2 w2} $l2 {
            assert_equal $id1 $id2
            assert_equal $w1 [r get weight_$id1]
            assert_equal $w2 [r get weight_$id1]
        }
    } {} {cluster:skip}

    test "SORT BY key STORE" {
        r sort tosort BY weight_* store sort-res
        assert_equal $result [r lrange sort-res 0 -1]
        assert_equal 16 [r llen sort-res]
        assert_encoding quicklist sort-res
    } {} {cluster:skip}

    test "SORT BY hash field STORE" {
        r sort tosort BY wobj_*->weight store sort-res
        assert_equal $result [r lrange sort-res 0 -1]
        assert_equal 16 [r llen sort-res]
        assert_encoding quicklist sort-res
    } {} {cluster:skip}

    test "SORT extracts STORE correctly" {
        r command getkeys sort abc store def
    } {abc def}

    test "SORT extracts multiple STORE correctly" {
        r command getkeys sort abc store invalid store stillbad store def
    } {abc def}

    test "SORT DESC" {
        assert_equal [lsort -decreasing -integer $result] [r sort tosort DESC]
    }

    test "SORT ALPHA against integer encoded strings" {
        r del mylist
        r lpush mylist 2
        r lpush mylist 1
        r lpush mylist 3
        r lpush mylist 10
        r sort mylist alpha
    } {1 10 2 3}

    test "SORT sorted set" {
        r del zset
        r zadd zset 1 a
        r zadd zset 5 b
        r zadd zset 2 c
        r zadd zset 10 d
        r zadd zset 3 e
        r sort zset alpha desc
    } {e d c b a}

    test "SORT sorted set BY nosort should retain ordering" {
        r del zset
        r zadd zset 1 a
        r zadd zset 5 b
        r zadd zset 2 c
        r zadd zset 10 d
        r zadd zset 3 e
        r multi
        r sort zset by nosort asc
        r sort zset by nosort desc
        r exec
    } {{a c e b d} {d b e c a}}

    test "SORT sorted set BY nosort + LIMIT" {
        r del zset
        r zadd zset 1 a
        r zadd zset 5 b
        r zadd zset 2 c
        r zadd zset 10 d
        r zadd zset 3 e
        assert_equal [r sort zset by nosort asc limit 0 1] {a}
        assert_equal [r sort zset by nosort desc limit 0 1] {d}
        assert_equal [r sort zset by nosort asc limit 0 2] {a c}
        assert_equal [r sort zset by nosort desc limit 0 2] {d b}
        assert_equal [r sort zset by nosort limit 5 10] {}
        assert_equal [r sort zset by nosort limit -10 100] {a c e b d}
    }

    test "SORT sorted set BY nosort works as expected from scripts" {
        r del zset
        r zadd zset 1 a
        r zadd zset 5 b
        r zadd zset 2 c
        r zadd zset 10 d
        r zadd zset 3 e
        r eval {
            return {redis.call('sort',KEYS[1],'by','nosort','asc'),
                    redis.call('sort',KEYS[1],'by','nosort','desc')}
        } 1 zset
    } {{a c e b d} {d b e c a}}

    test "SORT sorted set: +inf and -inf handling" {
        r del zset
        r zadd zset -100 a
        r zadd zset 200 b
        r zadd zset -300 c
        r zadd zset 1000000 d
        r zadd zset +inf max
        r zadd zset -inf min
        r zrange zset 0 -1
    } {min c a b d max}

    test "SORT regression for issue #19, sorting floats" {
        r flushdb
        set floats {1.1 5.10 3.10 7.44 2.1 5.75 6.12 0.25 1.15}
        foreach x $floats {
            r lpush mylist $x
        }
        assert_equal [lsort -real $floats] [r sort mylist]
    }

    test "SORT with STORE returns zero if result is empty (github issue 224)" {
        r flushdb
        r sort foo{t} store bar{t}
    } {0}

    test "SORT with STORE does not create empty lists (github issue 224)" {
        r flushdb
        r lpush foo{t} bar
        r sort foo{t} alpha limit 10 10 store zap{t}
        r exists zap{t}
    } {0}

    test "SORT with STORE removes key if result is empty (github issue 227)" {
        r flushdb
        r lpush foo{t} bar
        r sort emptylist{t} store foo{t}
        r exists foo{t}
    } {0}

    test "SORT with BY <constant> and STORE should still order output" {
        r del myset mylist
        r sadd myset a b c d e f g h i l m n o p q r s t u v z aa aaa azz
        r sort myset alpha by _ store mylist
        r lrange mylist 0 -1
    } {a aa aaa azz b c d e f g h i l m n o p q r s t u v z} {cluster:skip}

    test "SORT will complain with numerical sorting and bad doubles (1)" {
        r del myset
        r sadd myset 1 2 3 4 not-a-double
        set e {}
        catch {r sort myset} e
        set e
    } {*ERR*double*}

    test "SORT will complain with numerical sorting and bad doubles (2)" {
        r del myset
        r sadd myset 1 2 3 4
        r mset score:1 10 score:2 20 score:3 30 score:4 not-a-double
        set e {}
        catch {r sort myset by score:*} e
        set e
    } {*ERR*double*} {cluster:skip}

    test "SORT BY sub-sorts lexicographically if score is the same" {
        r del myset
        r sadd myset a b c d e f g h i l m n o p q r s t u v z aa aaa azz
        foreach ele {a aa aaa azz b c d e f g h i l m n o p q r s t u v z} {
            set score:$ele 100
        }
        r sort myset by score:*
    } {a aa aaa azz b c d e f g h i l m n o p q r s t u v z} {cluster:skip}

    test "SORT GET with pattern ending with just -> does not get hash field" {
        r del mylist
        r lpush mylist a
        r set x:a-> 100
        r sort mylist by num get x:*->
    } {100} {cluster:skip}

    test "SORT by nosort retains native order for lists" {
        r del testa
        r lpush testa 2 1 4 3 5
        r sort testa by nosort
    } {5 3 4 1 2} {cluster:skip}

    test "SORT by nosort plus store retains native order for lists" {
        r del testa
        r lpush testa 2 1 4 3 5
        r sort testa by nosort store testb
        r lrange testb 0 -1
    } {5 3 4 1 2} {cluster:skip}

    test "SORT by nosort with limit returns based on original list order" {
        r sort testa by nosort limit 0 3 store testb
        r lrange testb 0 -1
    } {5 3 4} {cluster:skip}

    tags {"slow"} {
        set num 100
        set res [create_random_dataset $num lpush]

        test "SORT speed, $num element list BY key, 100 times" {
            set start [clock clicks -milliseconds]
            for {set i 0} {$i < 100} {incr i} {
                set sorted [r sort tosort BY weight_* LIMIT 0 10]
            }
            set elapsed [expr [clock clicks -milliseconds]-$start]
            if {$::verbose} {
                puts -nonewline "\n  Average time to sort: [expr double($elapsed)/100] milliseconds "
                flush stdout
            }
        } {} {cluster:skip}

        test "SORT speed, $num element list BY hash field, 100 times" {
            set start [clock clicks -milliseconds]
            for {set i 0} {$i < 100} {incr i} {
                set sorted [r sort tosort BY wobj_*->weight LIMIT 0 10]
            }
            set elapsed [expr [clock clicks -milliseconds]-$start]
            if {$::verbose} {
                puts -nonewline "\n  Average time to sort: [expr double($elapsed)/100] milliseconds "
                flush stdout
            }
        } {} {cluster:skip}

        test "SORT speed, $num element list directly, 100 times" {
            set start [clock clicks -milliseconds]
            for {set i 0} {$i < 100} {incr i} {
                set sorted [r sort tosort LIMIT 0 10]
            }
            set elapsed [expr [clock clicks -milliseconds]-$start]
            if {$::verbose} {
                puts -nonewline "\n  Average time to sort: [expr double($elapsed)/100] milliseconds "
                flush stdout
            }
        }

        test "SORT speed, $num element list BY <const>, 100 times" {
            set start [clock clicks -milliseconds]
            for {set i 0} {$i < 100} {incr i} {
                set sorted [r sort tosort BY nokey LIMIT 0 10]
            }
            set elapsed [expr [clock clicks -milliseconds]-$start]
            if {$::verbose} {
                puts -nonewline "\n  Average time to sort: [expr double($elapsed)/100] milliseconds "
                flush stdout
            }
        } {} {cluster:skip}
    }
}
