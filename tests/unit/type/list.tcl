start_server {
    tags {"list"}
    overrides {
        "list-max-ziplist-value" 16
        "list-max-ziplist-entries" 256
    }
} {
    source "tests/unit/type/list-common.tcl"

    test {LPUSH, RPUSH, LLENGTH, LINDEX, LPOP - ziplist} {
        # first lpush then rpush
        assert_equal 1 [r lpush myziplist1 a]
        assert_equal 2 [r rpush myziplist1 b]
        assert_equal 3 [r rpush myziplist1 c]
        assert_equal 3 [r llen myziplist1]
        assert_equal a [r lindex myziplist1 0]
        assert_equal b [r lindex myziplist1 1]
        assert_equal c [r lindex myziplist1 2]
        assert_equal {} [r lindex myziplist2 3]
        assert_equal c [r rpop myziplist1]
        assert_equal a [r lpop myziplist1]
        assert_encoding ziplist myziplist1

        # first rpush then lpush
        assert_equal 1 [r rpush myziplist2 a]
        assert_equal 2 [r lpush myziplist2 b]
        assert_equal 3 [r lpush myziplist2 c]
        assert_equal 3 [r llen myziplist2]
        assert_equal c [r lindex myziplist2 0]
        assert_equal b [r lindex myziplist2 1]
        assert_equal a [r lindex myziplist2 2]
        assert_equal {} [r lindex myziplist2 3]
        assert_equal a [r rpop myziplist2]
        assert_equal c [r lpop myziplist2]
        assert_encoding ziplist myziplist2
    }

    test {LPUSH, RPUSH, LLENGTH, LINDEX, LPOP - regular list} {
        # first lpush then rpush
        assert_equal 1 [r lpush mylist1 $largevalue(linkedlist)]
        assert_encoding linkedlist mylist1
        assert_equal 2 [r rpush mylist1 b]
        assert_equal 3 [r rpush mylist1 c]
        assert_equal 3 [r llen mylist1]
        assert_equal $largevalue(linkedlist) [r lindex mylist1 0]
        assert_equal b [r lindex mylist1 1]
        assert_equal c [r lindex mylist1 2]
        assert_equal {} [r lindex mylist1 3]
        assert_equal c [r rpop mylist1]
        assert_equal $largevalue(linkedlist) [r lpop mylist1]

        # first rpush then lpush
        assert_equal 1 [r rpush mylist2 $largevalue(linkedlist)]
        assert_encoding linkedlist mylist2
        assert_equal 2 [r lpush mylist2 b]
        assert_equal 3 [r lpush mylist2 c]
        assert_equal 3 [r llen mylist2]
        assert_equal c [r lindex mylist2 0]
        assert_equal b [r lindex mylist2 1]
        assert_equal $largevalue(linkedlist) [r lindex mylist2 2]
        assert_equal {} [r lindex mylist2 3]
        assert_equal $largevalue(linkedlist) [r rpop mylist2]
        assert_equal c [r lpop mylist2]
    }

    test {R/LPOP against empty list} {
        r lpop non-existing-list
    } {}

    test {Variadic RPUSH/LPUSH} {
        r del mylist
        assert_equal 4 [r lpush mylist a b c d]
        assert_equal 8 [r rpush mylist 0 1 2 3]
        assert_equal {d c b a 0 1 2 3} [r lrange mylist 0 -1]
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

    proc create_linkedlist {key entries} {
        r del $key
        foreach entry $entries { r rpush $key $entry }
        assert_encoding linkedlist $key
    }

    foreach {type large} [array get largevalue] {
        test "BLPOP, BRPOP: single existing list - $type" {
            set rd [redis_deferring_client]
            create_$type blist "a b $large c d"

            $rd blpop blist 1
            assert_equal {blist a} [$rd read]
            $rd brpop blist 1
            assert_equal {blist d} [$rd read]

            $rd blpop blist 1
            assert_equal {blist b} [$rd read]
            $rd brpop blist 1
            assert_equal {blist c} [$rd read]
        }

        test "BLPOP, BRPOP: multiple existing lists - $type" {
            set rd [redis_deferring_client]
            create_$type blist1 "a $large c"
            create_$type blist2 "d $large f"

            $rd blpop blist1 blist2 1
            assert_equal {blist1 a} [$rd read]
            $rd brpop blist1 blist2 1
            assert_equal {blist1 c} [$rd read]
            assert_equal 1 [r llen blist1]
            assert_equal 3 [r llen blist2]

            $rd blpop blist2 blist1 1
            assert_equal {blist2 d} [$rd read]
            $rd brpop blist2 blist1 1
            assert_equal {blist2 f} [$rd read]
            assert_equal 1 [r llen blist1]
            assert_equal 1 [r llen blist2]
        }

        test "BLPOP, BRPOP: second list has an entry - $type" {
            set rd [redis_deferring_client]
            r del blist1
            create_$type blist2 "d $large f"

            $rd blpop blist1 blist2 1
            assert_equal {blist2 d} [$rd read]
            $rd brpop blist1 blist2 1
            assert_equal {blist2 f} [$rd read]
            assert_equal 0 [r llen blist1]
            assert_equal 1 [r llen blist2]
        }

        test "BRPOPLPUSH - $type" {
            r del target

            set rd [redis_deferring_client]
            create_$type blist "a b $large c d"

            $rd brpoplpush blist target 1
            assert_equal d [$rd read]

            assert_equal d [r rpop target]
            assert_equal "a b $large c" [r lrange blist 0 -1]
        }
    }

    test "BLPOP with variadic LPUSH" {
        set rd [redis_deferring_client]
        r del blist target
        if {$::valgrind} {after 100}
        $rd blpop blist 0
        if {$::valgrind} {after 100}
        assert_equal 2 [r lpush blist foo bar]
        if {$::valgrind} {after 100}
        assert_equal {blist foo} [$rd read]
        assert_equal bar [lindex [r lrange blist 0 -1] 0]
    }

    test "BRPOPLPUSH with zero timeout should block indefinitely" {
        set rd [redis_deferring_client]
        r del blist target
        $rd brpoplpush blist target 0
        after 1000
        r rpush blist foo
        assert_equal foo [$rd read]
        assert_equal {foo} [r lrange target 0 -1]
    }

    test "BRPOPLPUSH with a client BLPOPing the target list" {
        set rd [redis_deferring_client]
        set rd2 [redis_deferring_client]
        r del blist target
        $rd2 blpop target 0
        $rd brpoplpush blist target 0
        after 1000
        r rpush blist foo
        assert_equal foo [$rd read]
        assert_equal {target foo} [$rd2 read]
        assert_equal 0 [r exists target]
    }

    test "BRPOPLPUSH with wrong source type" {
        set rd [redis_deferring_client]
        r del blist target
        r set blist nolist
        $rd brpoplpush blist target 1
        assert_error "ERR*wrong kind*" {$rd read}
    }

    test "BRPOPLPUSH with wrong destination type" {
        set rd [redis_deferring_client]
        r del blist target
        r set target nolist
        r lpush blist foo
        $rd brpoplpush blist target 1
        assert_error "ERR*wrong kind*" {$rd read}

        set rd [redis_deferring_client]
        r del blist target
        r set target nolist
        $rd brpoplpush blist target 0
        after 1000
        r rpush blist foo
        assert_error "ERR*wrong kind*" {$rd read}
        assert_equal {foo} [r lrange blist 0 -1]
    }

    test "BRPOPLPUSH with multiple blocked clients" {
        set rd1 [redis_deferring_client]
        set rd2 [redis_deferring_client]
        r del blist target1 target2
        r set target1 nolist
        $rd1 brpoplpush blist target1 0
        $rd2 brpoplpush blist target2 0
        r lpush blist foo

        assert_error "ERR*wrong kind*" {$rd1 read}
        assert_equal {foo} [$rd2 read]
        assert_equal {foo} [r lrange target2 0 -1]
    }

    test "Linked BRPOPLPUSH" {
      set rd1 [redis_deferring_client]
      set rd2 [redis_deferring_client]

      r del list1 list2 list3

      $rd1 brpoplpush list1 list2 0
      $rd2 brpoplpush list2 list3 0

      r rpush list1 foo

      assert_equal {} [r lrange list1 0 -1]
      assert_equal {} [r lrange list2 0 -1]
      assert_equal {foo} [r lrange list3 0 -1]
    }

    test "Circular BRPOPLPUSH" {
      set rd1 [redis_deferring_client]
      set rd2 [redis_deferring_client]

      r del list1 list2

      $rd1 brpoplpush list1 list2 0
      $rd2 brpoplpush list2 list1 0

      r rpush list1 foo

      assert_equal {foo} [r lrange list1 0 -1]
      assert_equal {} [r lrange list2 0 -1]
    }

    test "Self-referential BRPOPLPUSH" {
      set rd [redis_deferring_client]

      r del blist

      $rd brpoplpush blist blist 0

      r rpush blist foo

      assert_equal {foo} [r lrange blist 0 -1]
    }

    test "BRPOPLPUSH inside a transaction" {
        r del xlist target
        r lpush xlist foo
        r lpush xlist bar

        r multi
        r brpoplpush xlist target 0
        r brpoplpush xlist target 0
        r brpoplpush xlist target 0
        r lrange xlist 0 -1
        r lrange target 0 -1
        r exec
    } {foo bar {} {} {bar foo}}

    test {BRPOPLPUSH timeout} {
      set rd [redis_deferring_client]

      $rd brpoplpush foo_list bar_list 1
      after 2000
      $rd read
    } {}

    foreach {pop} {BLPOP BRPOP} {
        test "$pop: with single empty list argument" {
            set rd [redis_deferring_client]
            r del blist1
            $rd $pop blist1 1
            r rpush blist1 foo
            assert_equal {blist1 foo} [$rd read]
            assert_equal 0 [r exists blist1]
        }

        test "$pop: with negative timeout" {
            set rd [redis_deferring_client]
            $rd $pop blist1 -1
            assert_error "ERR*is negative*" {$rd read}
        }

        test "$pop: with non-integer timeout" {
            set rd [redis_deferring_client]
            $rd $pop blist1 1.1
            assert_error "ERR*not an integer*" {$rd read}
        }

        test "$pop: with zero timeout should block indefinitely" {
            # To test this, use a timeout of 0 and wait a second.
            # The blocking pop should still be waiting for a push.
            set rd [redis_deferring_client]
            $rd $pop blist1 0
            after 1000
            r rpush blist1 foo
            assert_equal {blist1 foo} [$rd read]
        }

        test "$pop: second argument is not a list" {
            set rd [redis_deferring_client]
            r del blist1 blist2
            r set blist2 nolist
            $rd $pop blist1 blist2 1
            assert_error "ERR*wrong kind*" {$rd read}
        }

        test "$pop: timeout" {
            set rd [redis_deferring_client]
            r del blist1 blist2
            $rd $pop blist1 blist2 1
            assert_equal {} [$rd read]
        }

        test "$pop: arguments are empty" {
            set rd [redis_deferring_client]
            r del blist1 blist2

            $rd $pop blist1 blist2 1
            r rpush blist1 foo
            assert_equal {blist1 foo} [$rd read]
            assert_equal 0 [r exists blist1]
            assert_equal 0 [r exists blist2]

            $rd $pop blist1 blist2 1
            r rpush blist2 foo
            assert_equal {blist2 foo} [$rd read]
            assert_equal 0 [r exists blist1]
            assert_equal 0 [r exists blist2]
        }
    }

    test {BLPOP inside a transaction} {
        r del xlist
        r lpush xlist foo
        r lpush xlist bar
        r multi
        r blpop xlist 0
        r blpop xlist 0
        r blpop xlist 0
        r exec
    } {{xlist bar} {xlist foo} {}}

    test {LPUSHX, RPUSHX - generic} {
        r del xlist
        assert_equal 0 [r lpushx xlist a]
        assert_equal 0 [r llen xlist]
        assert_equal 0 [r rpushx xlist a]
        assert_equal 0 [r llen xlist]
    }

    foreach {type large} [array get largevalue] {
        test "LPUSHX, RPUSHX - $type" {
            create_$type xlist "$large c"
            assert_equal 3 [r rpushx xlist d]
            assert_equal 4 [r lpushx xlist a]
            assert_equal "a $large c d" [r lrange xlist 0 -1]
        }

        test "LINSERT - $type" {
            create_$type xlist "a $large c d"
            assert_equal 5 [r linsert xlist before c zz]
            assert_equal "a $large zz c d" [r lrange xlist 0 10]
            assert_equal 6 [r linsert xlist after c yy]
            assert_equal "a $large zz c yy d" [r lrange xlist 0 10]
            assert_equal 7 [r linsert xlist after d dd]
            assert_equal -1 [r linsert xlist after bad ddd]
            assert_equal "a $large zz c yy d dd" [r lrange xlist 0 10]
            assert_equal 8 [r linsert xlist before a aa]
            assert_equal -1 [r linsert xlist before bad aaa]
            assert_equal "aa a $large zz c yy d dd" [r lrange xlist 0 10]

            # check inserting integer encoded value
            assert_equal 9 [r linsert xlist before aa 42]
            assert_equal 42 [r lrange xlist 0 0]
        }
    }

    test {LINSERT raise error on bad syntax} {
        catch {[r linsert xlist aft3r aa 42]} e
        set e
    } {*ERR*syntax*error*}

    test {LPUSHX, RPUSHX convert from ziplist to list} {
        set large $largevalue(linkedlist)

        # convert when a large value is pushed
        create_ziplist xlist a
        assert_equal 2 [r rpushx xlist $large]
        assert_encoding linkedlist xlist
        create_ziplist xlist a
        assert_equal 2 [r lpushx xlist $large]
        assert_encoding linkedlist xlist

        # convert when the length threshold is exceeded
        create_ziplist xlist [lrepeat 256 a]
        assert_equal 257 [r rpushx xlist b]
        assert_encoding linkedlist xlist
        create_ziplist xlist [lrepeat 256 a]
        assert_equal 257 [r lpushx xlist b]
        assert_encoding linkedlist xlist
    }

    test {LINSERT convert from ziplist to list} {
        set large $largevalue(linkedlist)

        # convert when a large value is inserted
        create_ziplist xlist a
        assert_equal 2 [r linsert xlist before a $large]
        assert_encoding linkedlist xlist
        create_ziplist xlist a
        assert_equal 2 [r linsert xlist after a $large]
        assert_encoding linkedlist xlist

        # convert when the length threshold is exceeded
        create_ziplist xlist [lrepeat 256 a]
        assert_equal 257 [r linsert xlist before a a]
        assert_encoding linkedlist xlist
        create_ziplist xlist [lrepeat 256 a]
        assert_equal 257 [r linsert xlist after a a]
        assert_encoding linkedlist xlist

        # don't convert when the value could not be inserted
        create_ziplist xlist [lrepeat 256 a]
        assert_equal -1 [r linsert xlist before foo a]
        assert_encoding ziplist xlist
        create_ziplist xlist [lrepeat 256 a]
        assert_equal -1 [r linsert xlist after foo a]
        assert_encoding ziplist xlist
    }

    foreach {type num} {ziplist 250 linkedlist 500} {
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

    foreach {type large} [array get largevalue] {
        test "RPOPLPUSH base case - $type" {
            r del mylist1 mylist2
            create_$type mylist1 "a $large c d"
            assert_equal d [r rpoplpush mylist1 mylist2]
            assert_equal c [r rpoplpush mylist1 mylist2]
            assert_equal "a $large" [r lrange mylist1 0 -1]
            assert_equal "c d" [r lrange mylist2 0 -1]
            assert_encoding ziplist mylist2
        }

        test "RPOPLPUSH with the same list as src and dst - $type" {
            create_$type mylist "a $large c"
            assert_equal "a $large c" [r lrange mylist 0 -1]
            assert_equal c [r rpoplpush mylist mylist]
            assert_equal "c a $large" [r lrange mylist 0 -1]
        }

        foreach {othertype otherlarge} [array get largevalue] {
            test "RPOPLPUSH with $type source and existing target $othertype" {
                create_$type srclist "a b c $large"
                create_$othertype dstlist "$otherlarge"
                assert_equal $large [r rpoplpush srclist dstlist]
                assert_equal c [r rpoplpush srclist dstlist]
                assert_equal "a b" [r lrange srclist 0 -1]
                assert_equal "c $large $otherlarge" [r lrange dstlist 0 -1]

                # When we rpoplpush'ed a large value, dstlist should be
                # converted to the same encoding as srclist.
                if {$type eq "linkedlist"} {
                    assert_encoding linkedlist dstlist
                }
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

    foreach {type large} [array get largevalue] {
        test "Basic LPOP/RPOP - $type" {
            create_$type mylist "$large 1 2"
            assert_equal $large [r lpop mylist]
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

    foreach {type num} {ziplist 250 linkedlist 500} {
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

    foreach {type large} [array get largevalue] {
        test "LRANGE basics - $type" {
            create_$type mylist "$large 1 2 3 4 5 6 7 8 9"
            assert_equal {1 2 3 4 5 6 7 8} [r lrange mylist 1 -2]
            assert_equal {7 8 9} [r lrange mylist -3 -1]
            assert_equal {4} [r lrange mylist 4 4]
        }

        test "LRANGE inverted indexes - $type" {
            create_$type mylist "$large 1 2 3 4 5 6 7 8 9"
            assert_equal {} [r lrange mylist 6 2]
        }

        test "LRANGE out of range indexes including the full list - $type" {
            create_$type mylist "$large 1 2 3"
            assert_equal "$large 1 2 3" [r lrange mylist -1000 1000]
        }

        test "LRANGE out of range negative end index - $type" {
            create_$type mylist "$large 1 2 3"
            assert_equal $large [r lrange mylist 0 -4]
            assert_equal {} [r lrange mylist 0 -5]
        }
    }

    test {LRANGE against non existing key} {
        assert_equal {} [r lrange nosuchkey 0 1]
    }

    foreach {type large} [array get largevalue] {
        proc trim_list {type min max} {
            upvar 1 large large
            r del mylist
            create_$type mylist "1 2 3 4 $large"
            r ltrim mylist $min $max
            r lrange mylist 0 -1
        }

        test "LTRIM basics - $type" {
            assert_equal "1" [trim_list $type 0 0]
            assert_equal "1 2" [trim_list $type 0 1]
            assert_equal "1 2 3" [trim_list $type 0 2]
            assert_equal "2 3" [trim_list $type 1 2]
            assert_equal "2 3 4 $large" [trim_list $type 1 -1]
            assert_equal "2 3 4" [trim_list $type 1 -2]
            assert_equal "4 $large" [trim_list $type -2 -1]
            assert_equal "$large" [trim_list $type -1 -1]
            assert_equal "1 2 3 4 $large" [trim_list $type -5 -1]
            assert_equal "1 2 3 4 $large" [trim_list $type -10 10]
            assert_equal "1 2 3 4 $large" [trim_list $type 0 5]
            assert_equal "1 2 3 4 $large" [trim_list $type 0 10]
        }

        test "LTRIM out of range negative end index - $type" {
            assert_equal {1} [trim_list $type 0 -5]
            assert_equal {} [trim_list $type 0 -6]
        }

    }

    foreach {type large} [array get largevalue] {
        test "LSET - $type" {
            create_$type mylist "99 98 $large 96 95"
            r lset mylist 1 foo
            r lset mylist -1 bar
            assert_equal "99 foo $large 96 bar" [r lrange mylist 0 -1]
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

    foreach {type e} [array get largevalue] {
        test "LREM remove all the occurrences - $type" {
            create_$type mylist "$e foo bar foobar foobared zap bar test foo"
            assert_equal 2 [r lrem mylist 0 bar]
            assert_equal "$e foo foobar foobared zap test foo" [r lrange mylist 0 -1]
        }

        test "LREM remove the first occurrence - $type" {
            assert_equal 1 [r lrem mylist 1 foo]
            assert_equal "$e foobar foobared zap test foo" [r lrange mylist 0 -1]
        }

        test "LREM remove non existing element - $type" {
            assert_equal 0 [r lrem mylist 1 nosuchelement]
            assert_equal "$e foobar foobared zap test foo" [r lrange mylist 0 -1]
        }

        test "LREM starting from tail with negative count - $type" {
            create_$type mylist "$e foo bar foobar foobared zap bar test foo foo"
            assert_equal 1 [r lrem mylist -1 bar]
            assert_equal "$e foo bar foobar foobared zap test foo foo" [r lrange mylist 0 -1]
        }

        test "LREM starting from tail with negative count (2) - $type" {
            assert_equal 2 [r lrem mylist -2 foo]
            assert_equal "$e foo bar foobar foobared zap test" [r lrange mylist 0 -1]
        }

        test "LREM deleting objects that may be int encoded - $type" {
            create_$type myotherlist "$e 1 2 3"
            assert_equal 1 [r lrem myotherlist 1 2]
            assert_equal 3 [r llen myotherlist]
        }
    }

    test "Regression for bug 593 - chaining BRPOPLPUSH with other blocking cmds" {
        set rd1 [redis_deferring_client]
        set rd2 [redis_deferring_client]

        $rd1 brpoplpush a b 0
        $rd1 brpoplpush a b 0
        $rd2 brpoplpush b c 0
        after 1000
        r lpush a data
        $rd1 close
        $rd2 close
        r ping
    } {PONG}
}
