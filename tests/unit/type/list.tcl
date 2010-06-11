start_server {
    tags {"list"}
    overrides {
        "list-max-ziplist-value" 16
        "list-max-ziplist-entries" 256
    }
} {
    test {LPUSH, RPUSH, LLENGTH, LINDEX - ziplist} {
        # first lpush then rpush
        assert_equal 1 [r lpush myziplist1 a]
        assert_equal 2 [r rpush myziplist1 b]
        assert_equal 3 [r rpush myziplist1 c]
        assert_equal 3 [r llen myziplist1]
        assert_equal a [r lindex myziplist1 0]
        assert_equal b [r lindex myziplist1 1]
        assert_equal c [r lindex myziplist1 2]
        assert_encoding ziplist myziplist1

        # first rpush then lpush
        assert_equal 1 [r rpush myziplist2 a]
        assert_equal 2 [r lpush myziplist2 b]
        assert_equal 3 [r lpush myziplist2 c]
        assert_equal 3 [r llen myziplist2]
        assert_equal c [r lindex myziplist2 0]
        assert_equal b [r lindex myziplist2 1]
        assert_equal a [r lindex myziplist2 2]
        assert_encoding ziplist myziplist2
    }

    test {LPUSH, RPUSH, LLENGTH, LINDEX - regular list} {
        # use a string of length 17 to ensure a regular list is used
        set large_value "aaaaaaaaaaaaaaaaa"

        # first lpush then rpush
        assert_equal 1 [r lpush mylist1 $large_value]
        assert_encoding list mylist1
        assert_equal 2 [r rpush mylist1 b]
        assert_equal 3 [r rpush mylist1 c]
        assert_equal 3 [r llen mylist1]
        assert_equal $large_value [r lindex mylist1 0]
        assert_equal b [r lindex mylist1 1]
        assert_equal c [r lindex mylist1 2]

        # first rpush then lpush
        assert_equal 1 [r rpush mylist2 $large_value]
        assert_encoding list mylist2
        assert_equal 2 [r lpush mylist2 b]
        assert_equal 3 [r lpush mylist2 c]
        assert_equal 3 [r llen mylist2]
        assert_equal c [r lindex mylist2 0]
        assert_equal b [r lindex mylist2 1]
        assert_equal $large_value [r lindex mylist2 2]
    }

    test {DEL a list - ziplist} {
        assert_equal 1 [r del myziplist2]
        assert_equal 0 [r exists myziplist2]
        assert_equal 0 [r llen myziplist2]
    }

    test {DEL a list - regular list} {
        assert_equal 1 [r del mylist2]
        assert_equal 0 [r exists mylist2]
        assert_equal 0 [r llen mylist2]
    }

    proc create_ziplist {key entries} {
        r del $key
        foreach entry $entries { r rpush $key $entry }
        assert_encoding ziplist $key
    }

    proc create_list {key entries} {
        r del $key
        r rpush $key "aaaaaaaaaaaaaaaaa"
        foreach entry $entries { r rpush $key $entry }
        assert_equal "aaaaaaaaaaaaaaaaa" [r lpop $key]
        assert_encoding list $key
    }

    test {LPUSHX, RPUSHX - generic} {
        r del xlist
        assert_equal 0 [r lpushx xlist a]
        assert_equal 0 [r llen xlist]
        assert_equal 0 [r rpushx xlist a]
        assert_equal 0 [r llen xlist]
    }

    foreach type {ziplist list} {
        test "LPUSHX, RPUSHX - $type" {
            create_$type xlist {b c}
            assert_equal 3 [r rpushx xlist d]
            assert_equal 4 [r lpushx xlist a]
            assert_equal {a b c d} [r lrange xlist 0 -1]
        }

        test "LINSERT - $type" {
            create_$type xlist {a b c d}
            assert_equal 5 [r linsert xlist before c zz]
            assert_equal {a b zz c d} [r lrange xlist 0 10]
            assert_equal 6 [r linsert xlist after c yy]
            assert_equal {a b zz c yy d} [r lrange xlist 0 10]
            assert_equal 7 [r linsert xlist after d dd]
            assert_equal 7 [r linsert xlist after bad ddd]
            assert_equal {a b zz c yy d dd} [r lrange xlist 0 10]
            assert_equal 8 [r linsert xlist before a aa]
            assert_equal 8 [r linsert xlist before bad aaa]
            assert_equal {aa a b zz c yy d dd} [r lrange xlist 0 10]

            # check inserting integer encoded value
            assert_equal 9 [r linsert xlist before aa 42]
            assert_equal 42 [r lrange xlist 0 0]
        }
    }

    test {LPUSHX, RPUSHX convert from ziplist to list} {
        set large_value "aaaaaaaaaaaaaaaaa"

        # convert when a large value is pushed
        create_ziplist xlist a
        assert_equal 2 [r rpushx xlist $large_value]
        assert_encoding list xlist
        create_ziplist xlist a
        assert_equal 2 [r lpushx xlist $large_value]
        assert_encoding list xlist

        # convert when the length threshold is exceeded
        create_ziplist xlist [lrepeat 256 a]
        assert_equal 257 [r rpushx xlist b]
        assert_encoding list xlist
        create_ziplist xlist [lrepeat 256 a]
        assert_equal 257 [r lpushx xlist b]
        assert_encoding list xlist
    }

    test {LINSERT convert from ziplist to list} {
        set large_value "aaaaaaaaaaaaaaaaa"

        # convert when a large value is inserted
        create_ziplist xlist a
        assert_equal 2 [r linsert xlist before a $large_value]
        assert_encoding list xlist
        create_ziplist xlist a
        assert_equal 2 [r linsert xlist after a $large_value]
        assert_encoding list xlist

        # convert when the length threshold is exceeded
        create_ziplist xlist [lrepeat 256 a]
        assert_equal 257 [r linsert xlist before a a]
        assert_encoding list xlist
        create_ziplist xlist [lrepeat 256 a]
        assert_equal 257 [r linsert xlist after a a]
        assert_encoding list xlist

        # don't convert when the value could not be inserted
        create_ziplist xlist [lrepeat 256 a]
        assert_equal 256 [r linsert xlist before foo a]
        assert_encoding ziplist xlist
        create_ziplist xlist [lrepeat 256 a]
        assert_equal 256 [r linsert xlist after foo a]
        assert_encoding ziplist xlist
    }

    foreach {type num} {ziplist 250 list 500} {
        proc check_numbered_list_consistency {key} {
            set len [r llen $key]
            for {set i 0} {$i < $len} {incr i} {
                assert_equal $i [r lindex $key $i]
                assert_equal [expr $len-1-$i] [r lindex $key [expr (-$i)-1]]
            }
        }

        proc check_random_access_consistency {key} {
            set len [r llen $key]
            for {set i 0} {$i < $len} {incr i} {
                set rint [expr int(rand()*$len)]
                assert_equal $rint [r lindex $key $rint]
                assert_equal [expr $len-1-$rint] [r lindex $key [expr (-$rint)-1]]
            }
        }

        test "LINDEX consistency test - $type" {
            r del mylist
            for {set i 0} {$i < $num} {incr i} {
                r rpush mylist $i
            }
            assert_encoding $type mylist
            check_numbered_list_consistency mylist
        }

        test "LINDEX random access - $type" {
            assert_encoding $type mylist
            check_random_access_consistency mylist
        }

        test "Check if list is still ok after a DEBUG RELOAD - $type" {
            r debug reload
            assert_encoding $type mylist
            check_numbered_list_consistency mylist
            check_random_access_consistency mylist
        }
    }

    test {LLEN against non-list value error} {
        r del mylist
        r set mylist foobar
        assert_error ERR* {r llen mylist}
    }

    test {LLEN against non existing key} {
        assert_equal 0 [r llen not-a-key]
    }

    test {LINDEX against non-list value error} {
        assert_error ERR* {r lindex mylist 0}
    }

    test {LINDEX against non existing key} {
        assert_equal "" [r lindex not-a-key 10]
    }

    test {LPUSH against non-list value error} {
        assert_error ERR* {r lpush mylist 0}
    }

    test {RPUSH against non-list value error} {
        assert_error ERR* {r rpush mylist 0}
    }

    foreach type {ziplist list} {
        test "RPOPLPUSH base case - $type" {
            r del mylist1 mylist2
            create_$type mylist1 {a b c d}
            assert_equal d [r rpoplpush mylist1 mylist2]
            assert_equal c [r rpoplpush mylist1 mylist2]
            assert_equal {a b} [r lrange mylist1 0 -1]
            assert_equal {c d} [r lrange mylist2 0 -1]
            assert_encoding ziplist mylist2
        }

        test "RPOPLPUSH with the same list as src and dst - $type" {
            create_$type mylist {a b c}
            assert_equal {a b c} [r lrange mylist 0 -1]
            assert_equal c [r rpoplpush mylist mylist]
            assert_equal {c a b} [r lrange mylist 0 -1]
        }

        foreach othertype {ziplist list} {
            test "RPOPLPUSH with $type source and existing target $othertype" {
                create_$type srclist {a b c d}
                create_$othertype dstlist {x}
                assert_equal d [r rpoplpush srclist dstlist]
                assert_equal c [r rpoplpush srclist dstlist]
                assert_equal {a b} [r lrange srclist 0 -1]
                assert_equal {c d x} [r lrange dstlist 0 -1]
            }
        }
    }

    test {RPOPLPUSH against non existing key} {
        r del srclist dstlist
        assert_equal {} [r rpoplpush srclist dstlist]
        assert_equal 0 [r exists srclist]
        assert_equal 0 [r exists dstlist]
    }

    test {RPOPLPUSH against non list src key} {
        r del srclist dstlist
        r set srclist x
        assert_error ERR* {r rpoplpush srclist dstlist}
        assert_type string srclist
        assert_equal 0 [r exists newlist]
    }

    test {RPOPLPUSH against non list dst key} {
        create_ziplist srclist {a b c d}
        r set dstlist x
        assert_error ERR* {r rpoplpush srclist dstlist}
        assert_type string dstlist
        assert_equal {a b c d} [r lrange srclist 0 -1]
    }

    test {RPOPLPUSH against non existing src key} {
        r del srclist dstlist
        assert_equal {} [r rpoplpush srclist dstlist]
    } {}

    foreach type {ziplist list} {
        test "Basic LPOP/RPOP - $type" {
            create_$type mylist {0 1 2}
            assert_equal 0 [r lpop mylist]
            assert_equal 2 [r rpop mylist]
            assert_equal 1 [r lpop mylist]
            assert_equal 0 [r llen mylist]

            # pop on empty list
            assert_equal {} [r lpop mylist]
            assert_equal {} [r rpop mylist]
        }
    }

    test {LPOP/RPOP against non list value} {
        r set notalist foo
        assert_error ERR*kind* {r lpop notalist}
        assert_error ERR*kind* {r rpop notalist}
    }

    foreach {type num} {ziplist 250 list 500} {
        test "Mass RPOP/LPOP - $type" {
            r del mylist
            set sum1 0
            for {set i 0} {$i < $num} {incr i} {
                r lpush mylist $i
                incr sum1 $i
            }
            assert_encoding $type mylist
            set sum2 0
            for {set i 0} {$i < [expr $num/2]} {incr i} {
                incr sum2 [r lpop mylist]
                incr sum2 [r rpop mylist]
            }
            assert_equal $sum1 $sum2
        }
    }

    foreach type {ziplist list} {
        test "LRANGE basics - $type" {
            create_$type mylist {0 1 2 3 4 5 6 7 8 9}
            assert_equal {1 2 3 4 5 6 7 8} [r lrange mylist 1 -2]
            assert_equal {7 8 9} [r lrange mylist -3 -1]
            assert_equal {4} [r lrange mylist 4 4]
        }

        test "LRANGE inverted indexes - $type" {
            create_$type mylist {0 1 2 3 4 5 6 7 8 9}
            assert_equal {} [r lrange mylist 6 2]
        }

        test "LRANGE out of range indexes including the full list - $type" {
            create_$type mylist {1 2 3}
            assert_equal {1 2 3} [r lrange mylist -1000 1000]
        }
    }

    test {LRANGE against non existing key} {
        assert_equal {} [r lrange nosuchkey 0 1]
    }

    foreach type {ziplist list} {
        test "LTRIM basics - $type" {
            create_$type mylist "foo"
            for {set i 0} {$i < 100} {incr i} {
                r lpush mylist $i
                r ltrim mylist 0 4
            }
            r lrange mylist 0 -1
        } {99 98 97 96 95}

        test "LTRIM stress testing - $type" {
            set mylist {}
            for {set i 0} {$i < 20} {incr i} {
                lappend mylist $i
            }

            for {set j 0} {$j < 100} {incr j} {
                create_$type mylist $mylist

                # Trim at random
                set a [randomInt 20]
                set b [randomInt 20]
                r ltrim mylist $a $b
                assert_equal [lrange $mylist $a $b] [r lrange mylist 0 -1]
            }
        }

    }

    foreach type {ziplist list} {
        test "LSET - $type" {
            create_$type mylist {99 98 97 96 95}
            r lset mylist 1 foo
            r lset mylist -1 bar
            assert_equal {99 foo 97 96 bar} [r lrange mylist 0 -1]
        }

        test "LSET out of range index - $type" {
            assert_error ERR*range* {r lset mylist 10 foo}
        }
    }

    test {LSET against non existing key} {
        assert_error ERR*key* {r lset nosuchkey 10 foo}
    }

    test {LSET against non list value} {
        r set nolist foobar
        assert_error ERR*value* {r lset nolist 0 foo}
    }

    foreach type {ziplist list} {
        test "LREM remove all the occurrences - $type" {
            create_$type mylist {foo bar foobar foobared zap bar test foo}
            assert_equal 2 [r lrem mylist 0 bar]
            assert_equal {foo foobar foobared zap test foo} [r lrange mylist 0 -1]
        }

        test "LREM remove the first occurrence - $type" {
            assert_equal 1 [r lrem mylist 1 foo]
            assert_equal {foobar foobared zap test foo} [r lrange mylist 0 -1]
        }

        test "LREM remove non existing element - $type" {
            assert_equal 0 [r lrem mylist 1 nosuchelement]
            assert_equal {foobar foobared zap test foo} [r lrange mylist 0 -1]
        }

        test "LREM starting from tail with negative count - $type" {
            create_$type mylist {foo bar foobar foobared zap bar test foo foo}
            assert_equal 1 [r lrem mylist -1 bar]
            assert_equal {foo bar foobar foobared zap test foo foo} [r lrange mylist 0 -1]
        }

        test "LREM starting from tail with negative count (2) - $type" {
            assert_equal 2 [r lrem mylist -2 foo]
            assert_equal {foo bar foobar foobared zap test} [r lrange mylist 0 -1]
        }

        test "LREM deleting objects that may be int encoded - $type" {
            create_$type myotherlist {1 2 3}
            assert_equal 1 [r lrem myotherlist 1 2]
            assert_equal 2 [r llen myotherlist]
        }
    }
}
