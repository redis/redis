start_server {tags {"other"}} {
    if {$::force_failure} {
        # This is used just for test suite development purposes.
        test {Failing test} {
            format err
        } {ok}
    }

    test {BGSAVE} {
        waitForBgsave r
        r flushdb
        r save
        r set x 10
        r bgsave
        waitForBgsave r
        r debug reload
        r get x
    } {10}

    test {BGSAVE snapshot update values} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 1000
        set iterhalf 500
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
            r set $i $i
        }
        set mem1 [s used_memory]
        r bgsave
        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            set j [expr {$i + 5000}]
            r set $i $j
        }

        for {set i $iterhalf} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + 6000}]
            r set $i $j
        }
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            set v [r get $i]
            assert {$v == $i}
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot add new values} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 1000
        set iterhalf 500
        set iter2 1100
        set iter3 1200
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
            r set $i $i
        }
        set mem1 [s used_memory]
        r bgsave
        for {set i $iter1} {$i < $iter2} {incr i $step1} {
            r set $i $i
        }

        for {set i $iter2} {$i < $iter3} {incr i $step1} {
            r set $i $i
        }
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            set v [r get $i]
            assert {$v == $i}
        }
        for {set i $iter1} {$i < $iter3} {incr i $step1} {
            set v [r exists $i]
            assert {$v == 0}
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot delete values} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 1000
        set iterhalf 500
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
            r set $i $i
        }
        set mem1 [s used_memory]
        r bgsave
        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            r del $i
        }

        for {set i $iterhalf} {$i < $iter1} {incr i $step1} {
            r del $i
        }
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            set v [r get $i]
            assert {$v == $i}
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot append values} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 1000
        set iterhalf 500
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
            r set $i abcd
        }
        set mem1 [s used_memory]
        r bgsave
        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            r append $i xyz
        }

        for {set i $iterhalf} {$i < $iter1} {incr i $step1} {
            r append $i xyz
        }
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            set v [r get $i]
            assert_equal "$v"  "abcd"
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot setbit values} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 1000
        set iterhalf 500
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
            r set $i abcd
        }
        set mem1 [s used_memory]
        r bgsave
        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            r setbit $i 7 1
        }

        for {set i $iterhalf} {$i < $iter1} {incr i $step1} {
            r setbit $i 7 0
        }
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            set v [r get $i]
            assert_equal "$v"  "abcd"
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot setrange values} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 1000
        set iterhalf 500
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
            r set $i abcd
        }
        set mem1 [s used_memory]
        r bgsave
        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            r setrange $i 7 1
        }

        for {set i $iterhalf} {$i < $iter1} {incr i $step1} {
            r setrange $i 7 0
        }
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            set v [r get $i]
            assert_equal "$v"  "abcd"
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot incr & decr values} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 1000
        set iterhalf 500
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
            r set $i $i
        }
        set mem1 [s used_memory]
        r bgsave
        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            r incr $i
        }

        for {set i $iterhalf} {$i < $iter1} {incr i $step1} {
            r decr $i
        }
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            set v [r get $i]
            assert_equal "$v"  "$i"
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot incrby & decrby values} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 1000
        set iterhalf 500
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
            r set $i $i
        }
        set mem1 [s used_memory]
        r bgsave
        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            r incrby $i 3
        }

        for {set i $iterhalf} {$i < $iter1} {incr i $step1} {
            r decrby $i 3
        }
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            set v [r get $i]
            assert_equal "$v"  "$i"
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot rename values} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 1000
        set iterhalf 500
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
            r set $i $i
        }
        set mem1 [s used_memory]
        r bgsave
        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            set j [expr {$i + 6000}]
            r rename $i $j
        }

        for {set i $iterhalf} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + 6000}]
            r renamenx $i $j
        }
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            set v [r get $i]
            assert_equal "$v"  "$i"
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot move values} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 1000
        set iterhalf 500
        set step1 1
        r select 0
        for {set i 0} {$i < $iter1} {incr i $step1} {
            r set $i $i
        }
        r select 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + 1000}]
            r set $j $j
        }
        set mem1 [s used_memory]
        r bgsave
        r select 0
        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            r move $i 1
        }

        r select 1
        for {set i $iterhalf} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + 1000}]
            r move $j 0
        }
        waitForBgsave r
        r select 0
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            set v [r get $i]
            assert_equal "$v"  "$i"
        }
        r select 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + 1000}]
            set v [r get $j]
            assert_equal "$v"  "$j"
        }
        r flushdb
        r select 0
    } {OK}


    test {BGSAVE snapshot pop zip list} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 10
        set iterhalf 5
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
          r rpush mylist $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal $i [r lindex mylist $i]
        }
        set mem1 [s used_memory]
        after 15000
        r bgsave
        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            r rpop mylist
        }

        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            r lpop mylist
        }
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        assert_equal $iter1 [r llen mylist]
        for {set i 0} {$i < $iter1} {incr i $step1} {
            assert_equal $i [r lindex mylist $i]
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot push zip list} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 10
        set iterhalf 5
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
          r rpush mylist $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal $i [r lindex mylist $i]
        }
        set mem1 [s used_memory]
        r bgsave
        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            r lpush mylist abc
        }

        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            r rpush mylist xyz
        }
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        assert_equal $iter1 [r llen mylist]
        for {set i 0} {$i < $iter1} {incr i $step1} {
            assert_equal $i [r lindex mylist $i]
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot pushx zip list} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 10
        set iterhalf 5
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
          r rpush mylist $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal $i [r lindex mylist $i]
        }
        set mem1 [s used_memory]
        r bgsave
        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            r lpushx mylist abc
        }

        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            r rpushx mylist xyz
        }
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        assert_equal $iter1 [r llen mylist]
        for {set i 0} {$i < $iter1} {incr i $step1} {
            assert_equal $i [r lindex mylist $i]
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot blocking pop zip list} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 10
        set iterhalf 5
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
          r rpush mylist $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal $i [r lindex mylist $i]
        }
        set mem1 [s used_memory]
        r bgsave
        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            r brpop mylist 10
        }

        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            r blpop mylist 10
        }
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        assert_equal $iter1 [r llen mylist]
        for {set i 0} {$i < $iter1} {incr i $step1} {
            assert_equal $i [r lindex mylist $i]
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot insert zip list} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 10
        set iterhalf 5
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
          r rpush mylist $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal $i [r lindex mylist $i]
        }
        set mem1 [s used_memory]
        r bgsave
        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            r linsert mylist after 5 abc
        }

        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            r linsert mylist before 7 xyz
        }
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        assert_equal $iter1 [r llen mylist]
        for {set i 0} {$i < $iter1} {incr i $step1} {
            assert_equal $i [r lindex mylist $i]
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot rpoplpush zip list} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 10
        set iterhalf 5
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
          r rpush mylist $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          r rpush mylist2 $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal $i [r lindex mylist $i]
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal $i [r lindex mylist2 $i]
        }
        set mem1 [s used_memory]
        r bgsave
        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            r rpoplpush mylist mylist2
        }

        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            r brpoplpush mylist mylist2 10
        }
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        assert_equal $iter1 [r llen mylist]
        for {set i 0} {$i < $iter1} {incr i $step1} {
            assert_equal $i [r lindex mylist $i]
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal $i [r lindex mylist2 $i]
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot lset zip list} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 10
        set iterhalf 5
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
          r rpush mylist $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal $i [r lindex mylist $i]
        }
        set mem1 [s used_memory]
        r bgsave
        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            set j [expr {$i + 4000}]
            r lset mylist $i $j
        }

        for {set i $iterhalf} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + 4000}]
            r lset mylist $i $j
        }
        waitForBgsave r
        assert_equal $iter1 [r llen mylist]
        for {set i 0} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + 4000}]
            assert_equal $j [r lindex mylist $i]
        }
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        assert_equal $iter1 [r llen mylist]
        for {set i 0} {$i < $iter1} {incr i $step1} {
            assert_equal $i [r lindex mylist $i]
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot lrem zip list} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 10
        set iterhalf 5
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
          r rpush mylist $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal $i [r lindex mylist $i]
        }
        set mem1 [s used_memory]
        r bgsave
        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            r lrem mylist 0 $i
        }

        for {set i $iterhalf} {$i < $iter1} {incr i $step1} {
            r lrem mylist 0 $i
        }
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        assert_equal $iter1 [r llen mylist]
        for {set i 0} {$i < $iter1} {incr i $step1} {
            assert_equal $i [r lindex mylist $i]
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot ltrim zip list} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 10
        set iterhalf 5
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
          r rpush mylist $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal $i [r lindex mylist $i]
        }
        set mem1 [s used_memory]
        r bgsave
        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            r ltrim mylist $i $iter1
        }

        for {set i $iterhalf} {$i < $iter1} {incr i $step1} {
            r ltrim mylist 0 $i
        }
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        assert_equal $iter1 [r llen mylist]
        for {set i 0} {$i < $iter1} {incr i $step1} {
            assert_equal $i [r lindex mylist $i]
        }
        r flushdb
    } {OK}


    test {BGSAVE snapshot pop linked list} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 1000
        set iterhalf 500
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
          r rpush mylist $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal $i [r lindex mylist $i]
        }
        set mem1 [s used_memory]
        r bgsave
        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            r rpop mylist
        }

        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            r lpop mylist
        }
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        assert_equal $iter1 [r llen mylist]
        for {set i 0} {$i < $iter1} {incr i $step1} {
            assert_equal $i [r lindex mylist $i]
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot push linked list} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 1000
        set iterhalf 500
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
          r rpush mylist $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal $i [r lindex mylist $i]
        }
        set mem1 [s used_memory]
        r bgsave
        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            r lpush mylist abc
        }

        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            r rpush mylist xyz
        }
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        assert_equal $iter1 [r llen mylist]
        for {set i 0} {$i < $iter1} {incr i $step1} {
            assert_equal $i [r lindex mylist $i]
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot pushx linked list} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 1000
        set iterhalf 500
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
          r rpush mylist $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal $i [r lindex mylist $i]
        }
        set mem1 [s used_memory]
        r bgsave
        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            r lpushx mylist abc
        }

        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            r rpushx mylist xyz
        }
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        assert_equal $iter1 [r llen mylist]
        for {set i 0} {$i < $iter1} {incr i $step1} {
            assert_equal $i [r lindex mylist $i]
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot blocking pop linked list} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 1000
        set iterhalf 500
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
          r rpush mylist $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal $i [r lindex mylist $i]
        }
        set mem1 [s used_memory]
        r bgsave
        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            r brpop mylist 10
        }

        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            r blpop mylist 10
        }
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        assert_equal $iter1 [r llen mylist]
        for {set i 0} {$i < $iter1} {incr i $step1} {
            assert_equal $i [r lindex mylist $i]
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot insert linked list} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 1000
        set iterhalf 500
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
          r rpush mylist $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal $i [r lindex mylist $i]
        }
        set mem1 [s used_memory]
        r bgsave
        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            r linsert mylist after 10 abc
        }

        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            r linsert mylist before 20 xyz
        }
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        assert_equal $iter1 [r llen mylist]
        for {set i 0} {$i < $iter1} {incr i $step1} {
            assert_equal $i [r lindex mylist $i]
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot rpoplpush linked list} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 1000
        set iterhalf 500
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
          r rpush mylist $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          r rpush mylist2 $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal $i [r lindex mylist $i]
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal $i [r lindex mylist2 $i]
        }
        set mem1 [s used_memory]
        r bgsave
        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            r rpoplpush mylist mylist2
        }

        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            r brpoplpush mylist mylist2 10
        }
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        assert_equal $iter1 [r llen mylist]
        for {set i 0} {$i < $iter1} {incr i $step1} {
            assert_equal $i [r lindex mylist $i]
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal $i [r lindex mylist2 $i]
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot lset linked list} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 1000
        set iterhalf 500
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
          r rpush mylist $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal $i [r lindex mylist $i]
        }
        set mem1 [s used_memory]
        r bgsave
        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            set j [expr {$i + 4000}]
            r lset mylist $i $j
        }

        for {set i $iterhalf} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + 4000}]
            r lset mylist $i $j
        }
        waitForBgsave r
        assert_equal $iter1 [r llen mylist]
        for {set i 0} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + 4000}]
            assert_equal $j [r lindex mylist $i]
        }
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        assert_equal $iter1 [r llen mylist]
        for {set i 0} {$i < $iter1} {incr i $step1} {
            assert_equal $i [r lindex mylist $i]
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot lrem linked list} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 1000
        set iterhalf 500
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
          r rpush mylist $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal $i [r lindex mylist $i]
        }
        set mem1 [s used_memory]
        r bgsave
        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            r lrem mylist 0 $i
        }

        for {set i $iterhalf} {$i < $iter1} {incr i $step1} {
            r lrem mylist 0 $i
        }
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        assert_equal $iter1 [r llen mylist]
        for {set i 0} {$i < $iter1} {incr i $step1} {
            assert_equal $i [r lindex mylist $i]
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot ltrim linked list} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 1000
        set iterhalf 500
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
          r rpush mylist $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal $i [r lindex mylist $i]
        }
        set mem1 [s used_memory]
        r bgsave
        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            r ltrim mylist $i $iter1
        }

        for {set i $iterhalf} {$i < $iter1} {incr i $step1} {
            r ltrim mylist 0 $i
        }
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        assert_equal $iter1 [r llen mylist]
        for {set i 0} {$i < $iter1} {incr i $step1} {
            assert_equal $i [r lindex mylist $i]
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot sadd small set} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 10
        set iterhalf 5
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
            r sadd myset $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal 1 [r sismember myset $i]
        }
        set mem1 [s used_memory]
        r bgsave
        for {set i $iterhalf} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + 5000}]
            r sadd myset $j
        }

        for {set i 0} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + 6000}]
            r sadd myset $j
        }
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal 1 [r sismember myset $i]
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot srem small set} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 10
        set iterhalf 5
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
            r sadd myset $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal 1 [r sismember myset $i]
        }
        set mem1 [s used_memory]
        r bgsave
        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            r srem myset $i
        }

        for {set i $iterhalf} {$i < $iter1} {incr i $step1} {
            r srem myset $i
        }
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal 1 [r sismember myset $i]
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot smove small set} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 10
        set iterhalf 5
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
            r sadd myset $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + $iter1}]
            r sadd myset2 $j
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            assert_equal 1 [r sismember myset $i]
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + $iter1}]
            assert_equal 0 [r sismember myset2 $i]
            assert_equal 1 [r sismember myset2 $j]
        }
        set mem1 [s used_memory]
        r bgsave
        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            r smove myset myset2 $i
        }

        for {set i $iterhalf} {$i < $iter1} {incr i $step1} {
            r smove myset myset2 $i
        }
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            assert_equal 1 [r sismember myset $i]
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + $iter1}]
            assert_equal 0 [r sismember myset2 $i]
            assert_equal 1 [r sismember myset2 $j]
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot sinterstore small set} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 10
        set iterhalf 5
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
            r sadd myset $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + $i}]
            r sadd myset2 $j
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            assert_equal 1 [r sismember myset $i]
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + $i}]
            assert_equal 1 [r sismember myset2 $j]
        }
        set mem1 [s used_memory]
        r bgsave
        r sinterstore myset myset2 myset

        r sinterstore myset2 myset2 myset
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            assert_equal 1 [r sismember myset $i]
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + $i}]
            assert_equal 1 [r sismember myset2 $j]
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot sunionstore small set} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 10
        set iterhalf 5
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
            r sadd myset $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + $i}]
            r sadd myset2 $j
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            assert_equal 1 [r sismember myset $i]
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + $i}]
            assert_equal 1 [r sismember myset2 $j]
        }
        set mem1 [s used_memory]
        r bgsave
        r sunionstore myset myset2 myset

        r sunionstore myset2 myset2 myset
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            assert_equal 1 [r sismember myset $i]
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + $i}]
            assert_equal 1 [r sismember myset2 $j]
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot sdiffstore small set} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 10
        set iterhalf 5
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
            r sadd myset $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + $i}]
            r sadd myset2 $j
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            assert_equal 1 [r sismember myset $i]
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + $i}]
            assert_equal 1 [r sismember myset2 $j]
        }
        set mem1 [s used_memory]
        r bgsave
        r sdiffstore myset myset2 myset

        r sdiffstore myset2 myset2 myset
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            assert_equal 1 [r sismember myset $i]
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + $i}]
            assert_equal 1 [r sismember myset2 $j]
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot sadd large set} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 1000
        set iterhalf 500
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
            r sadd myset $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal 1 [r sismember myset $i]
        }
        set mem1 [s used_memory]
        r bgsave
        for {set i $iterhalf} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + 5000}]
            r sadd myset $j
        }

        for {set i 0} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + 6000}]
            r sadd myset $j
        }
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal 1 [r sismember myset $i]
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot srem large set} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 1000
        set iterhalf 500
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
            r sadd myset $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal 1 [r sismember myset $i]
        }
        set mem1 [s used_memory]
        r bgsave
        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            r srem myset $i
        }

        for {set i $iterhalf} {$i < $iter1} {incr i $step1} {
            r srem myset $i
        }
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal 1 [r sismember myset $i]
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot smove large set} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 1000
        set iterhalf 500
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
            r sadd myset $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + $iter1}]
            r sadd myset2 $j
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            assert_equal 1 [r sismember myset $i]
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + $iter1}]
            assert_equal 0 [r sismember myset2 $i]
            assert_equal 1 [r sismember myset2 $j]
        }
        set mem1 [s used_memory]
        r bgsave
        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            r smove myset myset2 $i
        }

        for {set i $iterhalf} {$i < $iter1} {incr i $step1} {
            r smove myset myset2 $i
        }
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            assert_equal 1 [r sismember myset $i]
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + $iter1}]
            assert_equal 0 [r sismember myset2 $i]
            assert_equal 1 [r sismember myset2 $j]
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot sinterstore large set} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 1000
        set iterhalf 500
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
            r sadd myset $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + $i}]
            r sadd myset2 $j
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            assert_equal 1 [r sismember myset $i]
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + $i}]
            assert_equal 1 [r sismember myset2 $j]
        }
        set mem1 [s used_memory]
        r bgsave
        r sinterstore myset myset2 myset

        r sinterstore myset2 myset2 myset
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            assert_equal 1 [r sismember myset $i]
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + $i}]
            assert_equal 1 [r sismember myset2 $j]
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot sunionstore large set} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 1000
        set iterhalf 500
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
            r sadd myset $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + $i}]
            r sadd myset2 $j
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            assert_equal 1 [r sismember myset $i]
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + $i}]
            assert_equal 1 [r sismember myset2 $j]
        }
        set mem1 [s used_memory]
        r bgsave
        r sunionstore myset myset2 myset

        r sunionstore myset2 myset2 myset
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            assert_equal 1 [r sismember myset $i]
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + $i}]
            assert_equal 1 [r sismember myset2 $j]
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot sdiffstore large set} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 1000
        set iterhalf 500
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
            r sadd myset $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + $i}]
            r sadd myset2 $j
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            assert_equal 1 [r sismember myset $i]
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + $i}]
            assert_equal 1 [r sismember myset2 $j]
        }
        set mem1 [s used_memory]
        r bgsave
        r sdiffstore myset myset2 myset

        r sdiffstore myset2 myset2 myset
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            assert_equal 1 [r sismember myset $i]
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + $i}]
            assert_equal 1 [r sismember myset2 $j]
        }
        r flushdb
    } {OK}


    test {BGSAVE snapshot zadd small ordered set} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 10
        set iterhalf 5
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
            r zadd myset $i $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal $i [r zscore myset $i]
        }
        set mem1 [s used_memory]
        r bgsave
        for {set i $iterhalf} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + 5000}]
            r zadd myset $j $j
        }

        for {set i 0} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + 6000}]
            r zadd myset $j $j
        }
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal $i [r zscore myset $i]
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot zrem small ordered set} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 10
        set iterhalf 5
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
            r zadd myset $i $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal $i [r zscore myset $i]
        }
        set mem1 [s used_memory]
        r bgsave
        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            r zrem myset $i
        }

        for {set i $iterhalf} {$i < $iter1} {incr i $step1} {
            r zrem myset $i
        }
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal $i [r zscore myset $i]
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot zincrby small ordered set} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 10
        set iterhalf 5
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
            r zadd myset $i $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal $i [r zscore myset $i]
        }
        set mem1 [s used_memory]
        r bgsave
        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            r zincrby myset $iter1 $i
        }

        for {set i $iterhalf} {$i < $iter1} {incr i $step1} {
            r zincrby myset $iter1 $i
        }
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal $i [r zscore myset $i]
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot zremrangebyscore small ordered set} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 10
        set iterhalf 5
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
            r zadd myset $i $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal $i [r zscore myset $i]
        }
        set mem1 [s used_memory]
        r bgsave
        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            r zremrangebyscore myset 0 $i
        }

        for {set i $iterhalf} {$i < $iter1} {incr i $step1} {
            r zremrangebyscore myset 0 $i
        }
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal $i [r zscore myset $i]
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot zremrangebyrank small ordered set} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 10
        set iterhalf 5
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
            r zadd myset $i $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal $i [r zscore myset $i]
        }
        set mem1 [s used_memory]
        r bgsave
        r zremrangebyrank myset 0 $iterhalf

        r zremrangebyrank myset 0 $iter1
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal $i [r zscore myset $i]
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot zunionstore small ordered set} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 10
        set iterhalf 5
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
            r zadd myset $i $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + $i}]
            r zadd myset2 $j $j
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            assert_equal $i [r zscore myset $i]
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + $i}]
            assert_equal $j [r zscore myset2 $j]
        }
        set mem1 [s used_memory]
        r bgsave
        r zunionstore myset 2 myset2 myset

        r zunionstore myset2 2 myset2 myset
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            assert_equal $i [r zscore myset $i]
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + $i}]
            assert_equal $j [r zscore myset2 $j]
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot zinterstore small ordered set} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 10
        set iterhalf 5
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
            r zadd myset $i $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + $i}]
            r zadd myset2 $j $j
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            assert_equal $i [r zscore myset $i]
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + $i}]
            assert_equal $j [r zscore myset2 $j]
        }
        set mem1 [s used_memory]
        r bgsave
        r zinterstore myset 2 myset2 myset

        r zinterstore myset2 2 myset2 myset
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            assert_equal $i [r zscore myset $i]
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + $i}]
            assert_equal $j [r zscore myset2 $j]
        }
        r flushdb
    } {OK}


    test {BGSAVE snapshot zadd large ordered set} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 1000
        set iterhalf 500
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
            r zadd myset $i $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal $i [r zscore myset $i]
        }
        set mem1 [s used_memory]
        r bgsave
        for {set i $iterhalf} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + 5000}]
            r zadd myset $j $j
        }

        for {set i 0} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + 6000}]
            r zadd myset $j $j
        }
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal $i [r zscore myset $i]
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot zrem large ordered set} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 1000
        set iterhalf 500
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
            r zadd myset $i $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal $i [r zscore myset $i]
        }
        set mem1 [s used_memory]
        r bgsave
        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            r zrem myset $i
        }

        for {set i $iterhalf} {$i < $iter1} {incr i $step1} {
            r zrem myset $i
        }
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal $i [r zscore myset $i]
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot zincrby large ordered set} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 1000
        set iterhalf 500
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
            r zadd myset $i $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal $i [r zscore myset $i]
        }
        set mem1 [s used_memory]
        r bgsave
        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            r zincrby myset $iter1 $i
        }

        for {set i $iterhalf} {$i < $iter1} {incr i $step1} {
            r zincrby myset $iter1 $i
        }
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal $i [r zscore myset $i]
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot zremrangebyscore large ordered set} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 1000
        set iterhalf 500
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
            r zadd myset $i $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal $i [r zscore myset $i]
        }
        set mem1 [s used_memory]
        r bgsave
        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            r zremrangebyscore myset 0 $i
        }

        for {set i $iterhalf} {$i < $iter1} {incr i $step1} {
            r zremrangebyscore myset 0 $i
        }
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal $i [r zscore myset $i]
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot zremrangebyrank large ordered set} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 1000
        set iterhalf 500
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
            r zadd myset $i $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal $i [r zscore myset $i]
        }
        set mem1 [s used_memory]
        r bgsave
        r zremrangebyrank myset 0 $iterhalf

        r zremrangebyrank myset 0 $iter1
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal $i [r zscore myset $i]
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot zunionstore large ordered set} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 1000
        set iterhalf 500
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
            r zadd myset $i $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + $i}]
            r zadd myset2 $j $j
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            assert_equal $i [r zscore myset $i]
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + $i}]
            assert_equal $j [r zscore myset2 $j]
        }
        set mem1 [s used_memory]
        r bgsave
        r zunionstore myset 2 myset2 myset

        r zunionstore myset2 2 myset2 myset
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            assert_equal $i [r zscore myset $i]
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + $i}]
            assert_equal $j [r zscore myset2 $j]
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot zinterstore large ordered set} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 1000
        set iterhalf 500
        set step1 1
        for {set i 0} {$i < $iter1} {incr i $step1} {
            r zadd myset $i $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + $i}]
            r zadd myset2 $j $j
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            assert_equal $i [r zscore myset $i]
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + $i}]
            assert_equal $j [r zscore myset2 $j]
        }
        set mem1 [s used_memory]
        r bgsave
        r zinterstore myset 2 myset2 myset

        r zinterstore myset2 2 myset2 myset
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            assert_equal $i [r zscore myset $i]
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + $i}]
            assert_equal $j [r zscore myset2 $j]
        }
        r flushdb
    } {OK}


    test {BGSAVE snapshot hdel small hash} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 10
        set iterhalf 5
        set step1 1
        set tot [expr {$iter1 + $iter1}]
        for {set i 0} {$i < $iter1} {incr i $step1} {
            r hset myhash $i $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal $i [r hget myhash $i]
        }
        set mem1 [s used_memory]
        r bgsave
        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            r hdel myhash $i
        }

        for {set i $iterhalf} {$i < $iter1} {incr i $step1} {
            r hdel myhash $i
        }
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        assert_equal $iter1 [r hlen myhash]
        for {set i 0} {$i < $iter1} {incr i $step1} {
            assert_equal $i [r hget myhash $i]
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot hset small hash} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 10
        set iterhalf 5
        set step1 1
        set tot [expr {$iter1 + $iter1}]
        for {set i 0} {$i < $iter1} {incr i $step1} {
            r hset myhash $i $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal $i [r hget myhash $i]
        }
        set mem1 [s used_memory]
        r bgsave
        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            set j [expr {$i + 5000}]
            r hset myhash $i $j
        }

        for {set i $iterhalf} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + 6000}]
            r hset myhash $i $j
        }
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        assert_equal $iter1 [r hlen myhash]
        for {set i 0} {$i < $iter1} {incr i $step1} {
            assert_equal $i [r hget myhash $i]
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot hsetnx small hash} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 10
        set iterhalf 5
        set step1 1
        set tot [expr {$iter1 + $iter1}]
        for {set i 0} {$i < $iter1} {incr i $step1} {
            r hset myhash $i $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal $i [r hget myhash $i]
        }
        set mem1 [s used_memory]
        r bgsave
        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            set j [expr {$i + 5000}]
            r hsetnx myhash $j $i
        }

        for {set i $iterhalf} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + 6000}]
            r hsetnx myhash $j $i
        }
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        assert_equal $iter1 [r hlen myhash]
        for {set i 0} {$i < $iter1} {incr i $step1} {
            assert_equal $i [r hget myhash $i]
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot hmset small hash} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 10
        set iterhalf 5
        set step1 1
        set tot [expr {$iter1 + $iter1}]
        for {set i 0} {$i < $iter1} {incr i $step1} {
            r hset myhash $i $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal $i [r hget myhash $i]
        }
        set mem1 [s used_memory]
        r bgsave
        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            set j [expr {$i + 5000}]
            r hmset myhash $i $j $j $j
        }

        for {set i $iterhalf} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + 6000}]
            r hmset myhash $i $j $j $j
        }
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        assert_equal $iter1 [r hlen myhash]
        for {set i 0} {$i < $iter1} {incr i $step1} {
            assert_equal $i [r hget myhash $i]
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot hincrby small hash} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 10
        set iterhalf 5
        set step1 1
        set tot [expr {$iter1 + $iter1}]
        for {set i 0} {$i < $iter1} {incr i $step1} {
            r hset myhash $i $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal $i [r hget myhash $i]
        }
        set mem1 [s used_memory]
        r bgsave
        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            r hincrby myhash $i 1000
        }

        for {set i $iterhalf} {$i < $iter1} {incr i $step1} {
            r hincrby myhash $i 2000
        }
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        assert_equal $iter1 [r hlen myhash]
        for {set i 0} {$i < $iter1} {incr i $step1} {
            assert_equal $i [r hget myhash $i]
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot hdel large hash} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 1000
        set iterhalf 500
        set step1 1
        set tot [expr {$iter1 + $iter1}]
        for {set i 0} {$i < $iter1} {incr i $step1} {
            r hset myhash $i $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal $i [r hget myhash $i]
        }
        set mem1 [s used_memory]
        r bgsave
        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            r hdel myhash $i
        }

        for {set i $iterhalf} {$i < $iter1} {incr i $step1} {
            r hdel myhash $i
        }
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        assert_equal $iter1 [r hlen myhash]
        for {set i 0} {$i < $iter1} {incr i $step1} {
            assert_equal $i [r hget myhash $i]
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot hset large hash} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 1000
        set iterhalf 500
        set step1 1
        set tot [expr {$iter1 + $iter1}]
        for {set i 0} {$i < $iter1} {incr i $step1} {
            r hset myhash $i $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal $i [r hget myhash $i]
        }
        set mem1 [s used_memory]
        r bgsave
        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            set j [expr {$i + 5000}]
            r hset myhash $i $j
        }

        for {set i $iterhalf} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + 6000}]
            r hset myhash $i $j
        }
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        assert_equal $iter1 [r hlen myhash]
        for {set i 0} {$i < $iter1} {incr i $step1} {
            assert_equal $i [r hget myhash $i]
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot hsetnx large hash} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 1000
        set iterhalf 500
        set step1 1
        set tot [expr {$iter1 + $iter1}]
        for {set i 0} {$i < $iter1} {incr i $step1} {
            r hset myhash $i $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal $i [r hget myhash $i]
        }
        set mem1 [s used_memory]
        r bgsave
        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            set j [expr {$i + 5000}]
            r hsetnx myhash $j $i
        }

        for {set i $iterhalf} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + 6000}]
            r hsetnx myhash $j $i
        }
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        assert_equal $iter1 [r hlen myhash]
        for {set i 0} {$i < $iter1} {incr i $step1} {
            assert_equal $i [r hget myhash $i]
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot hmset large hash} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 1000
        set iterhalf 500
        set step1 1
        set tot [expr {$iter1 + $iter1}]
        for {set i 0} {$i < $iter1} {incr i $step1} {
            r hset myhash $i $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal $i [r hget myhash $i]
        }
        set mem1 [s used_memory]
        r bgsave
        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            set j [expr {$i + 5000}]
            r hmset myhash $i $j $j $j
        }

        for {set i $iterhalf} {$i < $iter1} {incr i $step1} {
            set j [expr {$i + 6000}]
            r hmset myhash $i $j $j $j
        }
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        assert_equal $iter1 [r hlen myhash]
        for {set i 0} {$i < $iter1} {incr i $step1} {
            assert_equal $i [r hget myhash $i]
        }
        r flushdb
    } {OK}

    test {BGSAVE snapshot hincrby large hash} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 1000
        set iterhalf 500
        set step1 1
        set tot [expr {$iter1 + $iter1}]
        for {set i 0} {$i < $iter1} {incr i $step1} {
            r hset myhash $i $i
        }
        for {set i 0} {$i < $iter1} {incr i $step1} {
          assert_equal $i [r hget myhash $i]
        }
        set mem1 [s used_memory]
        r bgsave
        for {set i 0} {$i < $iterhalf} {incr i $step1} {
            r hincrby myhash $i 1000
        }

        for {set i $iterhalf} {$i < $iter1} {incr i $step1} {
            r hincrby myhash $i 2000
        }
        waitForBgsave r
        r debug flushload
        set mem2 [s used_memory]
        if {[expr {$mem2 - $mem1}] > 500} { puts "Warning: used memory before save $mem1 after flushload $mem2" }
        assert_equal $iter1 [r hlen myhash]
        for {set i 0} {$i < $iter1} {incr i $step1} {
            assert_equal $i [r hget myhash $i]
        }
        r flushdb
    } {OK}

# On Windows there are issues with expiring keys and the bgsave/flushload mechanism. 
# It looks like a race condition.
    test {BGSAVE expires} {
        waitForBgsave r
        r flushdb
        r set x 10
        r expire x 1000
        r bgsave
        r expire x 2

        waitForBgsave r
        r debug flushload
        set ttl [r ttl x]
        set e1 [expr {$ttl > 900 && $ttl <= 1000}]
        assert_equal $e1 1
        r flushdb
    } {OK}

    test {BGSAVE expires stress} {
        waitForBgsave r
        r flushdb
        r save
        set iter1 400
        set step1 1
		for {set rpt 0} {$rpt < 50} {incr rpt $step1} {
			for {set i 0} {$i < $iter1} {incr i $step1} {
				set exp [randomInt 4]
				incr exp
				r setex [randomKey] $exp $i
			}
			catch { r bgsave } err
			after 200
		}
        r flushdb
     } {OK}

}
