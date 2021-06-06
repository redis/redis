start_server {
    tags {"list"}
    overrides {
        "list-max-ziplist-size" 5
    }
} {
    source "tests/unit/type/list-common.tcl"

    test {LPOS basic usage} {
        r DEL mylist
        r RPUSH mylist a b c 1 2 3 c c
        assert {[r LPOS mylist a] == 0}
        assert {[r LPOS mylist c] == 2}
    }

    test {LPOS RANK (positive and negative rank) option} {
        assert {[r LPOS mylist c RANK 1] == 2}
        assert {[r LPOS mylist c RANK 2] == 6}
        assert {[r LPOS mylist c RANK 4] eq ""}
        assert {[r LPOS mylist c RANK -1] == 7}
        assert {[r LPOS mylist c RANK -2] == 6}
    }

    test {LPOS COUNT option} {
        assert {[r LPOS mylist c COUNT 0] == {2 6 7}}
        assert {[r LPOS mylist c COUNT 1] == {2}}
        assert {[r LPOS mylist c COUNT 2] == {2 6}}
        assert {[r LPOS mylist c COUNT 100] == {2 6 7}}
    }

    test {LPOS COUNT + RANK option} {
        assert {[r LPOS mylist c COUNT 0 RANK 2] == {6 7}}
        assert {[r LPOS mylist c COUNT 2 RANK -1] == {7 6}}
    }

    test {LPOS non existing key} {
        assert {[r LPOS mylistxxx c COUNT 0 RANK 2] eq {}}
    }

    test {LPOS no match} {
        assert {[r LPOS mylist x COUNT 2 RANK -1] eq {}}
        assert {[r LPOS mylist x RANK -1] eq {}}
    }

    test {LPOS MAXLEN} {
        assert {[r LPOS mylist a COUNT 0 MAXLEN 1] == {0}}
        assert {[r LPOS mylist c COUNT 0 MAXLEN 1] == {}}
        assert {[r LPOS mylist c COUNT 0 MAXLEN 3] == {2}}
        assert {[r LPOS mylist c COUNT 0 MAXLEN 3 RANK -1] == {7 6}}
        assert {[r LPOS mylist c COUNT 0 MAXLEN 7 RANK 2] == {6}}
    }

    test {LPOS when RANK is greater than matches} {
        r DEL mylist
        r LPUSH mylist a
        assert {[r LPOS mylist b COUNT 10 RANK 5] eq {}}
    }

    test {LPUSH, RPUSH, LLENGTH, LINDEX, LPOP - ziplist} {
        # first lpush then rpush
        assert_equal 1 [r lpush myziplist1 aa]
        assert_equal 2 [r rpush myziplist1 bb]
        assert_equal 3 [r rpush myziplist1 cc]
        assert_equal 3 [r llen myziplist1]
        assert_equal aa [r lindex myziplist1 0]
        assert_equal bb [r lindex myziplist1 1]
        assert_equal cc [r lindex myziplist1 2]
        assert_equal {} [r lindex myziplist2 3]
        assert_equal cc [r rpop myziplist1]
        assert_equal aa [r lpop myziplist1]
        assert_encoding quicklist myziplist1

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
        assert_encoding quicklist myziplist2
    }

    test {LPUSH, RPUSH, LLENGTH, LINDEX, LPOP - regular list} {
        # first lpush then rpush
        assert_equal 1 [r lpush mylist1 $largevalue(linkedlist)]
        assert_encoding quicklist mylist1
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
        assert_encoding quicklist mylist2
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
    
    test {R/LPOP with the optional count argument} {
        assert_equal 7 [r lpush listcount aa bb cc dd ee ff gg]
        assert_equal {} [r lpop listcount 0]
        assert_equal {gg} [r lpop listcount 1]
        assert_equal {ff ee} [r lpop listcount 2]
        assert_equal {aa bb} [r rpop listcount 2]
        assert_equal {cc} [r rpop listcount 1]
        assert_equal {dd} [r rpop listcount 123]
        assert_error "*ERR*range*" {r lpop forbarqaz -123}
    }

    test {Variadic RPUSH/LPUSH} {
        r del mylist
        assert_equal 4 [r lpush mylist a b c d]
        assert_equal 8 [r rpush mylist 0 1 2 3]
        assert_equal {d c b a 0 1 2 3} [r lrange mylist 0 -1]
    }

    test {DEL a list} {
        assert_equal 1 [r del mylist2]
        assert_equal 0 [r exists mylist2]
        assert_equal 0 [r llen mylist2]
    }

    proc create_list {key entries} {
        r del $key
        foreach entry $entries { r rpush $key $entry }
        assert_encoding quicklist $key
    }

    foreach {type large} [array get largevalue] {
        test "BLPOP, BRPOP: single existing list - $type" {
            set rd [redis_deferring_client]
            create_list blist "a b $large c d"

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
            create_list blist1{t} "a $large c"
            create_list blist2{t} "d $large f"

            $rd blpop blist1{t} blist2{t} 1
            assert_equal {blist1{t} a} [$rd read]
            $rd brpop blist1{t} blist2{t} 1
            assert_equal {blist1{t} c} [$rd read]
            assert_equal 1 [r llen blist1{t}]
            assert_equal 3 [r llen blist2{t}]

            $rd blpop blist2{t} blist1{t} 1
            assert_equal {blist2{t} d} [$rd read]
            $rd brpop blist2{t} blist1{t} 1
            assert_equal {blist2{t} f} [$rd read]
            assert_equal 1 [r llen blist1{t}]
            assert_equal 1 [r llen blist2{t}]
        }

        test "BLPOP, BRPOP: second list has an entry - $type" {
            set rd [redis_deferring_client]
            r del blist1{t}
            create_list blist2{t} "d $large f"

            $rd blpop blist1{t} blist2{t} 1
            assert_equal {blist2{t} d} [$rd read]
            $rd brpop blist1{t} blist2{t} 1
            assert_equal {blist2{t} f} [$rd read]
            assert_equal 0 [r llen blist1{t}]
            assert_equal 1 [r llen blist2{t}]
        }

        test "BRPOPLPUSH - $type" {
            r del target{t}
            r rpush target{t} bar

            set rd [redis_deferring_client]
            create_list blist{t} "a b $large c d"

            $rd brpoplpush blist{t} target{t} 1
            assert_equal d [$rd read]

            assert_equal d [r lpop target{t}]
            assert_equal "a b $large c" [r lrange blist{t} 0 -1]
        }

        foreach wherefrom {left right} {
            foreach whereto {left right} {
                test "BLMOVE $wherefrom $whereto - $type" {
                    r del target{t}
                    r rpush target{t} bar

                    set rd [redis_deferring_client]
                    create_list blist{t} "a b $large c d"

                    $rd blmove blist{t} target{t} $wherefrom $whereto 1
                    set poppedelement [$rd read]

                    if {$wherefrom eq "right"} {
                        assert_equal d $poppedelement
                        assert_equal "a b $large c" [r lrange blist{t} 0 -1]
                    } else {
                        assert_equal a $poppedelement
                        assert_equal "b $large c d" [r lrange blist{t} 0 -1]
                    }

                    if {$whereto eq "right"} {
                        assert_equal $poppedelement [r rpop target{t}]
                    } else {
                        assert_equal $poppedelement [r lpop target{t}]
                    }
                }
            }
        }
    }

    test "BLPOP, LPUSH + DEL should not awake blocked client" {
        set rd [redis_deferring_client]
        r del list

        $rd blpop list 0
        r multi
        r lpush list a
        r del list
        r exec
        r del list
        r lpush list b
        $rd read
    } {list b}

    test "BLPOP, LPUSH + DEL + SET should not awake blocked client" {
        set rd [redis_deferring_client]
        r del list

        $rd blpop list 0
        after 100 ;# Make sure rd is blocked before MULTI

        r multi
        r lpush list a
        r del list
        r set list foo
        r exec
        r del list
        r lpush list b
        $rd read
    } {list b}

    test "BLPOP with same key multiple times should work (issue #801)" {
        set rd [redis_deferring_client]
        r del list1{t} list2{t}

        # Data arriving after the BLPOP.
        $rd blpop list1{t} list2{t} list2{t} list1{t} 0
        r lpush list1{t} a
        assert_equal [$rd read] {list1{t} a}
        $rd blpop list1{t} list2{t} list2{t} list1{t} 0
        r lpush list2{t} b
        assert_equal [$rd read] {list2{t} b}

        # Data already there.
        r lpush list1{t} a
        r lpush list2{t} b
        $rd blpop list1{t} list2{t} list2{t} list1{t} 0
        assert_equal [$rd read] {list1{t} a}
        $rd blpop list1{t} list2{t} list2{t} list1{t} 0
        assert_equal [$rd read] {list2{t} b}
    }

    test "MULTI/EXEC is isolated from the point of view of BLPOP" {
        set rd [redis_deferring_client]
        r del list
        $rd blpop list 0
        r multi
        r lpush list a
        r lpush list b
        r lpush list c
        r exec
        $rd read
    } {list c}

    test "BLPOP with variadic LPUSH" {
        set rd [redis_deferring_client]
        r del blist
        if {$::valgrind} {after 100}
        $rd blpop blist 0
        if {$::valgrind} {after 100}
        assert_equal 2 [r lpush blist foo bar]
        if {$::valgrind} {after 100}
        assert_equal {blist bar} [$rd read]
        assert_equal foo [lindex [r lrange blist 0 -1] 0]
    }

    test "BRPOPLPUSH with zero timeout should block indefinitely" {
        set rd [redis_deferring_client]
        r del blist{t} target{t}
        r rpush target{t} bar
        $rd brpoplpush blist{t} target{t} 0
        wait_for_blocked_clients_count 1
        r rpush blist{t} foo
        assert_equal foo [$rd read]
        assert_equal {foo bar} [r lrange target{t} 0 -1]
    }

    foreach wherefrom {left right} {
        foreach whereto {left right} {
            test "BLMOVE $wherefrom $whereto with zero timeout should block indefinitely" {
                set rd [redis_deferring_client]
                r del blist{t} target{t}
                r rpush target{t} bar
                $rd blmove blist{t} target{t} $wherefrom $whereto 0
                wait_for_blocked_clients_count 1
                r rpush blist{t} foo
                assert_equal foo [$rd read]
                if {$whereto eq "right"} {
                    assert_equal {bar foo} [r lrange target{t} 0 -1]
                } else {
                    assert_equal {foo bar} [r lrange target{t} 0 -1]
                }
            }
        }
    }

    foreach wherefrom {left right} {
        foreach whereto {left right} {
            test "BLMOVE ($wherefrom, $whereto) with a client BLPOPing the target list" {
                set rd [redis_deferring_client]
                set rd2 [redis_deferring_client]
                r del blist{t} target{t}
                $rd2 blpop target{t} 0
                $rd blmove blist{t} target{t} $wherefrom $whereto 0
                wait_for_blocked_clients_count 2
                r rpush blist{t} foo
                assert_equal foo [$rd read]
                assert_equal {target{t} foo} [$rd2 read]
                assert_equal 0 [r exists target{t}]
            }
        }
    }

    test "BRPOPLPUSH with wrong source type" {
        set rd [redis_deferring_client]
        r del blist{t} target{t}
        r set blist{t} nolist
        $rd brpoplpush blist{t} target{t} 1
        assert_error "WRONGTYPE*" {$rd read}
    }

    test "BRPOPLPUSH with wrong destination type" {
        set rd [redis_deferring_client]
        r del blist{t} target{t}
        r set target{t} nolist
        r lpush blist{t} foo
        $rd brpoplpush blist{t} target{t} 1
        assert_error "WRONGTYPE*" {$rd read}

        set rd [redis_deferring_client]
        r del blist{t} target{t}
        r set target{t} nolist
        $rd brpoplpush blist{t} target{t} 0
        wait_for_blocked_clients_count 1
        r rpush blist{t} foo
        assert_error "WRONGTYPE*" {$rd read}
        assert_equal {foo} [r lrange blist{t} 0 -1]
    }

    test "BRPOPLPUSH maintains order of elements after failure" {
        set rd [redis_deferring_client]
        r del blist{t} target{t}
        r set target{t} nolist
        $rd brpoplpush blist{t} target{t} 0
        r rpush blist{t} a b c
        assert_error "WRONGTYPE*" {$rd read}
        r lrange blist{t} 0 -1
    } {a b c}

    test "BRPOPLPUSH with multiple blocked clients" {
        set rd1 [redis_deferring_client]
        set rd2 [redis_deferring_client]
        r del blist{t} target1{t} target2{t}
        r set target1{t} nolist
        $rd1 brpoplpush blist{t} target1{t} 0
        $rd2 brpoplpush blist{t} target2{t} 0
        r lpush blist{t} foo

        assert_error "WRONGTYPE*" {$rd1 read}
        assert_equal {foo} [$rd2 read]
        assert_equal {foo} [r lrange target2{t} 0 -1]
    }

    test "Linked LMOVEs" {
      set rd1 [redis_deferring_client]
      set rd2 [redis_deferring_client]

      r del list1{t} list2{t} list3{t}

      $rd1 blmove list1{t} list2{t} right left 0
      $rd2 blmove list2{t} list3{t} left right 0

      r rpush list1{t} foo

      assert_equal {} [r lrange list1{t} 0 -1]
      assert_equal {} [r lrange list2{t} 0 -1]
      assert_equal {foo} [r lrange list3{t} 0 -1]
    }

    test "Circular BRPOPLPUSH" {
      set rd1 [redis_deferring_client]
      set rd2 [redis_deferring_client]

      r del list1{t} list2{t}

      $rd1 brpoplpush list1{t} list2{t} 0
      $rd2 brpoplpush list2{t} list1{t} 0

      r rpush list1{t} foo

      assert_equal {foo} [r lrange list1{t} 0 -1]
      assert_equal {} [r lrange list2{t} 0 -1]
    }

    test "Self-referential BRPOPLPUSH" {
      set rd [redis_deferring_client]

      r del blist{t}

      $rd brpoplpush blist{t} blist{t} 0

      r rpush blist{t} foo

      assert_equal {foo} [r lrange blist{t} 0 -1]
    }

    test "BRPOPLPUSH inside a transaction" {
        r del xlist{t} target{t}
        r lpush xlist{t} foo
        r lpush xlist{t} bar

        r multi
        r brpoplpush xlist{t} target{t} 0
        r brpoplpush xlist{t} target{t} 0
        r brpoplpush xlist{t} target{t} 0
        r lrange xlist{t} 0 -1
        r lrange target{t} 0 -1
        r exec
    } {foo bar {} {} {bar foo}}

    test "PUSH resulting from BRPOPLPUSH affect WATCH" {
        set blocked_client [redis_deferring_client]
        set watching_client [redis_deferring_client]
        r del srclist{t} dstlist{t} somekey{t}
        r set somekey{t} somevalue
        $blocked_client brpoplpush srclist{t} dstlist{t} 0
        $watching_client watch dstlist{t}
        $watching_client read
        $watching_client multi
        $watching_client read
        $watching_client get somekey{t}
        $watching_client read
        r lpush srclist{t} element
        $watching_client exec
        $watching_client read
    } {}

    test "BRPOPLPUSH does not affect WATCH while still blocked" {
        set blocked_client [redis_deferring_client]
        set watching_client [redis_deferring_client]
        r del srclist{t} dstlist{t} somekey{t}
        r set somekey{t} somevalue
        $blocked_client brpoplpush srclist{t} dstlist{t} 0
        $watching_client watch dstlist{t}
        $watching_client read
        $watching_client multi
        $watching_client read
        $watching_client get somekey{t}
        $watching_client read
        $watching_client exec
        # Blocked BLPOPLPUSH may create problems, unblock it.
        r lpush srclist{t} element
        $watching_client read
    } {somevalue}

    test {BRPOPLPUSH timeout} {
      set rd [redis_deferring_client]

      $rd brpoplpush foo_list{t} bar_list{t} 1
      wait_for_blocked_clients_count 1
      wait_for_blocked_clients_count 0 500 10
      $rd read
    } {}

    test "BLPOP when new key is moved into place" {
        set rd [redis_deferring_client]

        $rd blpop foo{t} 5
        r lpush bob{t} abc def hij
        r rename bob{t} foo{t}
        $rd read
    } {foo{t} hij}

    test "BLPOP when result key is created by SORT..STORE" {
        set rd [redis_deferring_client]

        # zero out list from previous test without explicit delete
        r lpop foo{t}
        r lpop foo{t}
        r lpop foo{t}

        $rd blpop foo{t} 5
        r lpush notfoo{t} hello hola aguacate konichiwa zanzibar
        r sort notfoo{t} ALPHA store foo{t}
        $rd read
    } {foo{t} aguacate}

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
            r del blist1
            $rd $pop blist1 0.1
            r rpush blist1 foo
            assert_equal {blist1 foo} [$rd read]
            assert_equal 0 [r exists blist1]
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
            r del blist1{t} blist2{t}
            r set blist2{t} nolist{t}
            $rd $pop blist1{t} blist2{t} 1
            assert_error "WRONGTYPE*" {$rd read}
        }

        test "$pop: timeout" {
            set rd [redis_deferring_client]
            r del blist1{t} blist2{t}
            $rd $pop blist1{t} blist2{t} 1
            assert_equal {} [$rd read]
        }

        test "$pop: arguments are empty" {
            set rd [redis_deferring_client]
            r del blist1{t} blist2{t}

            $rd $pop blist1{t} blist2{t} 1
            r rpush blist1{t} foo
            assert_equal {blist1{t} foo} [$rd read]
            assert_equal 0 [r exists blist1{t}]
            assert_equal 0 [r exists blist2{t}]

            $rd $pop blist1{t} blist2{t} 1
            r rpush blist2{t} foo
            assert_equal {blist2{t} foo} [$rd read]
            assert_equal 0 [r exists blist1{t}]
            assert_equal 0 [r exists blist2{t}]
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
            create_list xlist "$large c"
            assert_equal 3 [r rpushx xlist d]
            assert_equal 4 [r lpushx xlist a]
            assert_equal 6 [r rpushx xlist 42 x]
            assert_equal 9 [r lpushx xlist y3 y2 y1]
            assert_equal "y1 y2 y3 a $large c d 42 x" [r lrange xlist 0 -1]
        }

        test "LINSERT - $type" {
            create_list xlist "a $large c d"
            assert_equal 5 [r linsert xlist before c zz] "before c"
            assert_equal "a $large zz c d" [r lrange xlist 0 10] "lrangeA"
            assert_equal 6 [r linsert xlist after c yy] "after c"
            assert_equal "a $large zz c yy d" [r lrange xlist 0 10] "lrangeB"
            assert_equal 7 [r linsert xlist after d dd] "after d"
            assert_equal -1 [r linsert xlist after bad ddd] "after bad"
            assert_equal "a $large zz c yy d dd" [r lrange xlist 0 10] "lrangeC"
            assert_equal 8 [r linsert xlist before a aa] "before a"
            assert_equal -1 [r linsert xlist before bad aaa] "before bad"
            assert_equal "aa a $large zz c yy d dd" [r lrange xlist 0 10] "lrangeD"

            # check inserting integer encoded value
            assert_equal 9 [r linsert xlist before aa 42] "before aa"
            assert_equal 42 [r lrange xlist 0 0] "lrangeE"
        }
    }

    test {LINSERT raise error on bad syntax} {
        catch {[r linsert xlist aft3r aa 42]} e
        set e
    } {*ERR*syntax*error*}

    foreach {type num} {quicklist 250 quicklist 500} {
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
        } {} {needs:debug}
    }

    test {LLEN against non-list value error} {
        r del mylist
        r set mylist foobar
        assert_error WRONGTYPE* {r llen mylist}
    }

    test {LLEN against non existing key} {
        assert_equal 0 [r llen not-a-key]
    }

    test {LINDEX against non-list value error} {
        assert_error WRONGTYPE* {r lindex mylist 0}
    }

    test {LINDEX against non existing key} {
        assert_equal "" [r lindex not-a-key 10]
    }

    test {LPUSH against non-list value error} {
        assert_error WRONGTYPE* {r lpush mylist 0}
    }

    test {RPUSH against non-list value error} {
        assert_error WRONGTYPE* {r rpush mylist 0}
    }

    foreach {type large} [array get largevalue] {
        test "RPOPLPUSH base case - $type" {
            r del mylist1{t} mylist2{t}
            create_list mylist1{t} "a $large c d"
            assert_equal d [r rpoplpush mylist1{t} mylist2{t}]
            assert_equal c [r rpoplpush mylist1{t} mylist2{t}]
            assert_equal "a $large" [r lrange mylist1{t} 0 -1]
            assert_equal "c d" [r lrange mylist2{t} 0 -1]
            assert_encoding quicklist mylist2{t}
        }

        foreach wherefrom {left right} {
            foreach whereto {left right} {
                test "LMOVE $wherefrom $whereto base case - $type" {
                    r del mylist1{t} mylist2{t}

                    if {$wherefrom eq "right"} {
                        create_list mylist1{t} "c d $large a"
                    } else {
                        create_list mylist1{t} "a $large c d"
                    }
                    assert_equal a [r lmove mylist1{t} mylist2{t} $wherefrom $whereto]
                    assert_equal $large [r lmove mylist1{t} mylist2{t} $wherefrom $whereto]
                    assert_equal "c d" [r lrange mylist1{t} 0 -1]
                    if {$whereto eq "right"} {
                        assert_equal "a $large" [r lrange mylist2{t} 0 -1]
                    } else {
                        assert_equal "$large a" [r lrange mylist2{t} 0 -1]
                    }
                    assert_encoding quicklist mylist2{t}
                }
            }
        }

        test "RPOPLPUSH with the same list as src and dst - $type" {
            create_list mylist{t} "a $large c"
            assert_equal "a $large c" [r lrange mylist{t} 0 -1]
            assert_equal c [r rpoplpush mylist{t} mylist{t}]
            assert_equal "c a $large" [r lrange mylist{t} 0 -1]
        }

        foreach wherefrom {left right} {
            foreach whereto {left right} {
                test "LMOVE $wherefrom $whereto with the same list as src and dst - $type" {
                    if {$wherefrom eq "right"} {
                        create_list mylist{t} "a $large c"
                        assert_equal "a $large c" [r lrange mylist{t} 0 -1]
                    } else {
                        create_list mylist{t} "c a $large"
                        assert_equal "c a $large" [r lrange mylist{t} 0 -1]
                    }
                    assert_equal c [r lmove mylist{t} mylist{t} $wherefrom $whereto]
                    if {$whereto eq "right"} {
                        assert_equal "a $large c" [r lrange mylist{t} 0 -1]
                    } else {
                        assert_equal "c a $large" [r lrange mylist{t} 0 -1]
                    }
                }
            }
        }

        foreach {othertype otherlarge} [array get largevalue] {
            test "RPOPLPUSH with $type source and existing target $othertype" {
                create_list srclist{t} "a b c $large"
                create_list dstlist{t} "$otherlarge"
                assert_equal $large [r rpoplpush srclist{t} dstlist{t}]
                assert_equal c [r rpoplpush srclist{t} dstlist{t}]
                assert_equal "a b" [r lrange srclist{t} 0 -1]
                assert_equal "c $large $otherlarge" [r lrange dstlist{t} 0 -1]

                # When we rpoplpush'ed a large value, dstlist should be
                # converted to the same encoding as srclist.
                if {$type eq "linkedlist"} {
                    assert_encoding quicklist dstlist{t}
                }
            }

            foreach wherefrom {left right} {
                foreach whereto {left right} {
                    test "LMOVE $wherefrom $whereto with $type source and existing target $othertype" {
                        create_list dstlist{t} "$otherlarge"

                        if {$wherefrom eq "right"} {
                            create_list srclist{t} "a b c $large"
                        } else {
                            create_list srclist{t} "$large c a b"
                        }
                        assert_equal $large [r lmove srclist{t} dstlist{t} $wherefrom $whereto]
                        assert_equal c [r lmove srclist{t} dstlist{t} $wherefrom $whereto]
                        assert_equal "a b" [r lrange srclist{t} 0 -1]

                        if {$whereto eq "right"} {
                            assert_equal "$otherlarge $large c" [r lrange dstlist{t} 0 -1]
                        } else {
                            assert_equal "c $large $otherlarge" [r lrange dstlist{t} 0 -1]
                        }

                        # When we lmoved a large value, dstlist should be
                        # converted to the same encoding as srclist.
                        if {$type eq "linkedlist"} {
                            assert_encoding quicklist dstlist{t}
                        }
                    }
                }
            }
        }
    }

    test {RPOPLPUSH against non existing key} {
        r del srclist{t} dstlist{t}
        assert_equal {} [r rpoplpush srclist{t} dstlist{t}]
        assert_equal 0 [r exists srclist{t}]
        assert_equal 0 [r exists dstlist{t}]
    }

    test {RPOPLPUSH against non list src key} {
        r del srclist{t} dstlist{t}
        r set srclist{t} x
        assert_error WRONGTYPE* {r rpoplpush srclist{t} dstlist{t}}
        assert_type string srclist{t}
        assert_equal 0 [r exists newlist{t}]
    }

    test {RPOPLPUSH against non list dst key} {
        create_list srclist{t} {a b c d}
        r set dstlist{t} x
        assert_error WRONGTYPE* {r rpoplpush srclist{t} dstlist{t}}
        assert_type string dstlist{t}
        assert_equal {a b c d} [r lrange srclist{t} 0 -1]
    }

    test {RPOPLPUSH against non existing src key} {
        r del srclist{t} dstlist{t}
        assert_equal {} [r rpoplpush srclist{t} dstlist{t}]
    } {}

    foreach {type large} [array get largevalue] {
        test "Basic LPOP/RPOP - $type" {
            create_list mylist "$large 1 2"
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
        assert_error WRONGTYPE* {r lpop notalist}
        assert_error WRONGTYPE* {r rpop notalist}
    }

    foreach {type num} {quicklist 250 quicklist 500} {
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
            create_list mylist "$large 1 2 3 4 5 6 7 8 9"
            assert_equal {1 2 3 4 5 6 7 8} [r lrange mylist 1 -2]
            assert_equal {7 8 9} [r lrange mylist -3 -1]
            assert_equal {4} [r lrange mylist 4 4]
        }

        test "LRANGE inverted indexes - $type" {
            create_list mylist "$large 1 2 3 4 5 6 7 8 9"
            assert_equal {} [r lrange mylist 6 2]
        }

        test "LRANGE out of range indexes including the full list - $type" {
            create_list mylist "$large 1 2 3"
            assert_equal "$large 1 2 3" [r lrange mylist -1000 1000]
        }

        test "LRANGE out of range negative end index - $type" {
            create_list mylist "$large 1 2 3"
            assert_equal $large [r lrange mylist 0 -4]
            assert_equal {} [r lrange mylist 0 -5]
        }
    }

    test {LRANGE against non existing key} {
        assert_equal {} [r lrange nosuchkey 0 1]
    }

    test {LRANGE with start > end yields an empty array for backward compatibility} {
        create_list mylist "1 2 3"
        assert_equal {} [r lrange mylist 1 0]
        assert_equal {} [r lrange mylist -1 -2]
    }

    foreach {type large} [array get largevalue] {
        proc trim_list {type min max} {
            upvar 1 large large
            r del mylist
            create_list mylist "1 2 3 4 $large"
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
            create_list mylist "99 98 $large 96 95"
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
        assert_error WRONGTYPE* {r lset nolist 0 foo}
    }

    foreach {type e} [array get largevalue] {
        test "LREM remove all the occurrences - $type" {
            create_list mylist "$e foo bar foobar foobared zap bar test foo"
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
            create_list mylist "$e foo bar foobar foobared zap bar test foo foo"
            assert_equal 1 [r lrem mylist -1 bar]
            assert_equal "$e foo bar foobar foobared zap test foo foo" [r lrange mylist 0 -1]
        }

        test "LREM starting from tail with negative count (2) - $type" {
            assert_equal 2 [r lrem mylist -2 foo]
            assert_equal "$e foo bar foobar foobared zap test" [r lrange mylist 0 -1]
        }

        test "LREM deleting objects that may be int encoded - $type" {
            create_list myotherlist "$e 1 2 3"
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

    test "client unblock tests" {
        r del l
        set rd [redis_deferring_client]
        $rd client id
        set id [$rd read]

        # test default args
        $rd blpop l 0
        wait_for_blocked_client
        r client unblock $id
        assert_equal {} [$rd read]

        # test with timeout
        $rd blpop l 0
        wait_for_blocked_client
        r client unblock $id TIMEOUT
        assert_equal {} [$rd read]

        # test with error
        $rd blpop l 0
        wait_for_blocked_client
        r client unblock $id ERROR
        catch {[$rd read]} e
        assert_equal $e "UNBLOCKED client unblocked via CLIENT UNBLOCK"

        # test with invalid client id
        catch {[r client unblock asd]} e
        assert_equal $e "ERR value is not an integer or out of range"

        # test with non blocked client
        set myid [r client id]
        catch {[r client unblock $myid]} e
        assert_equal $e {invalid command name "0"}

        # finally, see the this client and list are still functional
        $rd blpop l 0
        wait_for_blocked_client
        r lpush l foo
        assert_equal {l foo} [$rd read]
    } {}

    test {List ziplist of various encodings} {
        r del k
        r lpush k 127 ;# ZIP_INT_8B
        r lpush k 32767 ;# ZIP_INT_16B
        r lpush k 2147483647 ;# ZIP_INT_32B
        r lpush k 9223372036854775808 ;# ZIP_INT_64B
        r lpush k 0 ;# ZIP_INT_IMM_MIN
        r lpush k 12 ;# ZIP_INT_IMM_MAX
        r lpush k [string repeat x 31] ;# ZIP_STR_06B
        r lpush k [string repeat x 8191] ;# ZIP_STR_14B
        r lpush k [string repeat x 65535] ;# ZIP_STR_32B
        set k [r lrange k 0 -1]
        set dump [r dump k]

        config_set sanitize-dump-payload no mayfail
        r restore kk 0 $dump
        set kk [r lrange kk 0 -1]

        # try some forward and backward searches to make sure all encodings
        # can be traversed
        assert_equal [r lindex kk 5] {9223372036854775808}
        assert_equal [r lindex kk -5] {0}
        assert_equal [r lpos kk foo rank 1] {}
        assert_equal [r lpos kk foo rank -1] {}

        # make sure the values are right
        assert_equal $k $kk
        assert_equal [lpop k] [string repeat x 65535]
        assert_equal [lpop k] [string repeat x 8191]
        assert_equal [lpop k] [string repeat x 31]
        set _ $k
    } {12 0 9223372036854775808 2147483647 32767 127}

    test {List ziplist of various encodings - sanitize dump} {
        config_set sanitize-dump-payload yes mayfail
        r restore kk 0 $dump replace
        set k [r lrange k 0 -1]
        set kk [r lrange kk 0 -1]

        # make sure the values are right
        assert_equal $k $kk
        assert_equal [lpop k] [string repeat x 65535]
        assert_equal [lpop k] [string repeat x 8191]
        assert_equal [lpop k] [string repeat x 31]
        set _ $k
    } {12 0 9223372036854775808 2147483647 32767 127}

}
