start_server {tags {"list"}} {
    test {Basic LPUSH, RPUSH, LLENGTH, LINDEX} {
        set res [r lpush mylist a]
        append res [r lpush mylist b]
        append res [r rpush mylist c]
        append res [r llen mylist]
        append res [r rpush anotherlist d]
        append res [r lpush anotherlist e]
        append res [r llen anotherlist]
        append res [r lindex mylist 0]
        append res [r lindex mylist 1]
        append res [r lindex mylist 2]
        append res [r lindex anotherlist 0]
        append res [r lindex anotherlist 1]
        list $res [r lindex mylist 100]
    } {1233122baced {}}

    test {DEL a list} {
        r del mylist
        r exists mylist
    } {0}

    proc create_list {key entries} {
        r del $key
        foreach entry $entries { r rpush $key $entry }
    }

    test "BLPOP, BRPOP: single existing list" {
        set rd [redis_deferring_client]
        create_list blist {a b c d}

        $rd blpop blist 1
        assert_equal {blist a} [$rd read]
        $rd brpop blist 1
        assert_equal {blist d} [$rd read]

        $rd blpop blist 1
        assert_equal {blist b} [$rd read]
        $rd brpop blist 1
        assert_equal {blist c} [$rd read]
    }

    test "BLPOP, BRPOP: multiple existing lists" {
        set rd [redis_deferring_client]
        create_list blist1 {a b c}
        create_list blist2 {d e f}

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

    test "BLPOP, BRPOP: second list has an entry" {
        set rd [redis_deferring_client]
        r del blist1
        create_list blist2 {d e f}

        $rd blpop blist1 blist2 1
        assert_equal {blist2 d} [$rd read]
        $rd brpop blist1 blist2 1
        assert_equal {blist2 f} [$rd read]
        assert_equal 0 [r llen blist1]
        assert_equal 1 [r llen blist2]
    }

    foreach {pop} {BLPOP BRPOP} {
        test "$pop: with single empty list argument" {
            set rd [redis_deferring_client]
            r del blist1
            $rd $pop blist1 1
            r rpush blist1 foo
            assert_equal {blist1 foo} [$rd read]
            assert_equal 0 [r exists blist1]
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

    test {Create a long list and check every single element with LINDEX} {
        set ok 0
        for {set i 0} {$i < 1000} {incr i} {
            r rpush mylist $i
        }
        for {set i 0} {$i < 1000} {incr i} {
            if {[r lindex mylist $i] eq $i} {incr ok}
            if {[r lindex mylist [expr (-$i)-1]] eq [expr 999-$i]} {
                incr ok
            }
        }
        format $ok
    } {2000}

    test {Test elements with LINDEX in random access} {
        set ok 0
        for {set i 0} {$i < 1000} {incr i} {
            set rint [expr int(rand()*1000)]
            if {[r lindex mylist $rint] eq $rint} {incr ok}
            if {[r lindex mylist [expr (-$rint)-1]] eq [expr 999-$rint]} {
                incr ok
            }
        }
        format $ok
    } {2000}

    test {Check if the list is still ok after a DEBUG RELOAD} {
        r debug reload
        set ok 0
        for {set i 0} {$i < 1000} {incr i} {
            set rint [expr int(rand()*1000)]
            if {[r lindex mylist $rint] eq $rint} {incr ok}
            if {[r lindex mylist [expr (-$rint)-1]] eq [expr 999-$rint]} {
                incr ok
            }
        }
        format $ok
    } {2000}

    test {LLEN against non-list value error} {
        r del mylist
        r set mylist foobar
        catch {r llen mylist} err
        format $err
    } {ERR*}

    test {LLEN against non existing key} {
        r llen not-a-key
    } {0}

    test {LINDEX against non-list value error} {
        catch {r lindex mylist 0} err
        format $err
    } {ERR*}

    test {LINDEX against non existing key} {
        r lindex not-a-key 10
    } {}

    test {LPUSH against non-list value error} {
        catch {r lpush mylist 0} err
        format $err
    } {ERR*}

    test {RPUSH against non-list value error} {
        catch {r rpush mylist 0} err
        format $err
    } {ERR*}

    test {RPOPLPUSH base case} {
        r del mylist
        r rpush mylist a
        r rpush mylist b
        r rpush mylist c
        r rpush mylist d
        set v1 [r rpoplpush mylist newlist]
        set v2 [r rpoplpush mylist newlist]
        set l1 [r lrange mylist 0 -1]
        set l2 [r lrange newlist 0 -1]
        list $v1 $v2 $l1 $l2
    } {d c {a b} {c d}}

    test {RPOPLPUSH with the same list as src and dst} {
        r del mylist
        r rpush mylist a
        r rpush mylist b
        r rpush mylist c
        set l1 [r lrange mylist 0 -1]
        set v [r rpoplpush mylist mylist]
        set l2 [r lrange mylist 0 -1]
        list $l1 $v $l2
    } {{a b c} c {c a b}}

    test {RPOPLPUSH target list already exists} {
        r del mylist
        r del newlist
        r rpush mylist a
        r rpush mylist b
        r rpush mylist c
        r rpush mylist d
        r rpush newlist x
        set v1 [r rpoplpush mylist newlist]
        set v2 [r rpoplpush mylist newlist]
        set l1 [r lrange mylist 0 -1]
        set l2 [r lrange newlist 0 -1]
        list $v1 $v2 $l1 $l2
    } {d c {a b} {c d x}}

    test {RPOPLPUSH against non existing key} {
        r del mylist
        r del newlist
        set v1 [r rpoplpush mylist newlist]
        list $v1 [r exists mylist] [r exists newlist]
    } {{} 0 0}

    test {RPOPLPUSH against non list src key} {
        r del mylist
        r del newlist
        r set mylist x
        catch {r rpoplpush mylist newlist} err
        list [r type mylist] [r exists newlist] [string range $err 0 2]
    } {string 0 ERR}

    test {RPOPLPUSH against non list dst key} {
        r del mylist
        r del newlist
        r rpush mylist a
        r rpush mylist b
        r rpush mylist c
        r rpush mylist d
        r set newlist x
        catch {r rpoplpush mylist newlist} err
        list [r lrange mylist 0 -1] [r type newlist] [string range $err 0 2]
    } {{a b c d} string ERR}

    test {RPOPLPUSH against non existing src key} {
        r del mylist
        r del newlist
        r rpoplpush mylist newlist
    } {}
    
    test {Basic LPOP/RPOP} {
        r del mylist
        r rpush mylist 1
        r rpush mylist 2
        r lpush mylist 0
        list [r lpop mylist] [r rpop mylist] [r lpop mylist] [r llen mylist]
    } [list 0 2 1 0]

    test {LPOP/RPOP against empty list} {
        r lpop mylist
    } {}

    test {LPOP against non list value} {
        r set notalist foo
        catch {r lpop notalist} err
        format $err
    } {ERR*kind*}

    test {Mass LPUSH/LPOP} {
        set sum 0
        for {set i 0} {$i < 1000} {incr i} {
            r lpush mylist $i
            incr sum $i
        }
        set sum2 0
        for {set i 0} {$i < 500} {incr i} {
            incr sum2 [r lpop mylist]
            incr sum2 [r rpop mylist]
        }
        expr $sum == $sum2
    } {1}

    test {LRANGE basics} {
        for {set i 0} {$i < 10} {incr i} {
            r rpush mylist $i
        }
        list [r lrange mylist 1 -2] \
                [r lrange mylist -3 -1] \
                [r lrange mylist 4 4]
    } {{1 2 3 4 5 6 7 8} {7 8 9} 4}

    test {LRANGE inverted indexes} {
        r lrange mylist 6 2
    } {}

    test {LRANGE out of range indexes including the full list} {
        r lrange mylist -1000 1000
    } {0 1 2 3 4 5 6 7 8 9}

    test {LRANGE out of range negative end index} {
        list [r lrange mylist 0 -10] [r lrange mylist 0 -11]
    } {0 {}}

    test {LRANGE against non existing key} {
        r lrange nosuchkey 0 1
    } {}

    test {LTRIM basics} {
        r del mylist
        for {set i 0} {$i < 100} {incr i} {
            r lpush mylist $i
            r ltrim mylist 0 4
        }
        r lrange mylist 0 -1
    } {99 98 97 96 95}

    test {LTRIM with out of range negative end index} {
        r ltrim mylist 0 -6
        r llen mylist
    } {0}

    test {LTRIM stress testing} {
        set mylist {}
        set err {}
        for {set i 0} {$i < 20} {incr i} {
            lappend mylist $i
        }

        for {set j 0} {$j < 100} {incr j} {
            # Fill the list
            r del mylist
            for {set i 0} {$i < 20} {incr i} {
                r rpush mylist $i
            }
            # Trim at random
            set a [randomInt 20]
            set b [randomInt 20]
            r ltrim mylist $a $b
            if {[r lrange mylist 0 -1] ne [lrange $mylist $a $b]} {
                set err "[r lrange mylist 0 -1] != [lrange $mylist $a $b]"
                break
            }
        }
        set _ $err
    } {}

    test {LSET} {
        r del mylist
        foreach x {99 98 97 96 95} {
            r rpush mylist $x
        }
        r lset mylist 1 foo
        r lset mylist -1 bar
        r lrange mylist 0 -1
    } {99 foo 97 96 bar}

    test {LSET out of range index} {
        catch {r lset mylist 10 foo} err
        format $err
    } {ERR*range*}

    test {LSET against non existing key} {
        catch {r lset nosuchkey 10 foo} err
        format $err
    } {ERR*key*}

    test {LSET against non list value} {
        r set nolist foobar
        catch {r lset nolist 0 foo} err
        format $err
    } {ERR*value*}
    
    test {LREM, remove all the occurrences} {
        r flushdb
        r rpush mylist foo
        r rpush mylist bar
        r rpush mylist foobar
        r rpush mylist foobared
        r rpush mylist zap
        r rpush mylist bar
        r rpush mylist test
        r rpush mylist foo
        set res [r lrem mylist 0 bar]
        list [r lrange mylist 0 -1] $res
    } {{foo foobar foobared zap test foo} 2}

    test {LREM, remove the first occurrence} {
        set res [r lrem mylist 1 foo]
        list [r lrange mylist 0 -1] $res
    } {{foobar foobared zap test foo} 1}

    test {LREM, remove non existing element} {
        set res [r lrem mylist 1 nosuchelement]
        list [r lrange mylist 0 -1] $res
    } {{foobar foobared zap test foo} 0}

    test {LREM, starting from tail with negative count} {
        r flushdb
        r rpush mylist foo
        r rpush mylist bar
        r rpush mylist foobar
        r rpush mylist foobared
        r rpush mylist zap
        r rpush mylist bar
        r rpush mylist test
        r rpush mylist foo
        r rpush mylist foo
        set res [r lrem mylist -1 bar]
        list [r lrange mylist 0 -1] $res
    } {{foo bar foobar foobared zap test foo foo} 1}

    test {LREM, starting from tail with negative count (2)} {
        set res [r lrem mylist -2 foo]
        list [r lrange mylist 0 -1] $res
    } {{foo bar foobar foobared zap test} 2}

    test {LREM, deleting objects that may be encoded as integers} {
        r lpush myotherlist 1
        r lpush myotherlist 2
        r lpush myotherlist 3
        r lrem myotherlist 1 2
        r llen myotherlist
    } {2}
}
