start_server {
    tags {"sort"}
    overrides {
        "list-max-ziplist-value" 16
        "list-max-ziplist-entries" 32
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
        16 lpush ziplist "Ziplist"
        1000 lpush linkedlist "Linked list"
        10000 lpush linkedlist "Big Linked list"
        16 sadd intset "Intset"
        1000 sadd hashtable "Hash table"
        10000 sadd hashtable "Big Hash table"
    } {
        set result [create_random_dataset $num $cmd]
        assert_encoding $enc tosort

        test "$title: SORT BY key" {
            assert_equal $result [r sort tosort BY weight_*]
        }

        test "$title: SORT BY hash field" {
            assert_equal $result [r sort tosort BY wobj_*->weight]
        }
    }

    set result [create_random_dataset 16 lpush]
    test "SORT GET #" {
        assert_equal [lsort -integer $result] [r sort tosort GET #]
    }

    test "SORT GET <const>" {
        r del foo
        set res [r sort tosort GET foo]
        assert_equal 16 [llength $res]
        foreach item $res { assert_equal {} $item }
    }

    test "SORT GET (key and hash) with sanity check" {
        set l1 [r sort tosort GET # GET weight_*]
        set l2 [r sort tosort GET # GET wobj_*->weight]
        foreach {id1 w1} $l1 {id2 w2} $l2 {
            assert_equal $id1 $id2
            assert_equal $w1 [r get weight_$id1]
            assert_equal $w2 [r get weight_$id1]
        }
    }

    test "SORT BY key STORE" {
        r sort tosort BY weight_* store sort-res
        assert_equal $result [r lrange sort-res 0 -1]
        assert_equal 16 [r llen sort-res]
        assert_encoding ziplist sort-res
    }

    test "SORT BY hash field STORE" {
        r sort tosort BY wobj_*->weight store sort-res
        assert_equal $result [r lrange sort-res 0 -1]
        assert_equal 16 [r llen sort-res]
        assert_encoding ziplist sort-res
    }
   
   test "SORT multiple BY integer DESC BY strings ALPHA" {
        r del mylist
        r lpush mylist 2
        r lpush mylist 1
        r lpush mylist 3
        r lpush mylist 10
        r set weight:2 0
        r set weight:1 0
        r set weight:3 0
        r set weight:10 1
        r set name:2 aaa
        r set name:1 bbb
        r set name:3 ccc
        r set name:10 ddd
        r sort mylist by weight:* desc by name:* alpha get name:*
    } {ddd aaa bbb ccc}

    test "SORT multiple BY integer BY strings ALPHA" {
        r set weight:10 -3
        r sort mylist by weight:* asc by name:* alpha get name:*
    } {ddd aaa bbb ccc}

    test "SORT multiple BY integer BY strings ALPHA DESC" {
        r sort mylist by weight:* asc by name:* alpha desc get name:*
    } {ddd ccc bbb aaa}

    test "SORT multiple BY integer DESC BY strings ALPHA DESC" {
        r set weight:10 100 
        r sort mylist by weight:* asc by name:* alpha desc get name:*
    } {ccc bbb aaa ddd}
    
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

    test "SORT with STORE returns zero if result is empty (github isse 224)" {
        r flushdb
        r sort foo store bar
    } {0}

    test "SORT with STORE does not create empty lists (github issue 224)" {
        r flushdb
        r lpush foo bar
        r sort foo limit 10 10 store zap
        r exists zap
    } {0}

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
        }

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
        }

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
        }
    }
}
