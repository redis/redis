start_server {tags {"scan"}} {
    test "SCAN basic" {
        r flushdb
        r debug populate 1000

        set cur 0
        set keys {}
        while 1 {
            set res [r scan $cur]
            set cur [lindex $res 0]
            set k [lindex $res 1]
            lappend keys {*}$k
            if {$cur == 0} break
        }

        set keys [lsort -unique $keys]
        assert_equal 1000 [llength $keys]
    }

    test "SCAN COUNT" {
        r flushdb
        r debug populate 1000

        set cur 0
        set keys {}
        while 1 {
            set res [r scan $cur count 5]
            set cur [lindex $res 0]
            set k [lindex $res 1]
            lappend keys {*}$k
            if {$cur == 0} break
        }

        set keys [lsort -unique $keys]
        assert_equal 1000 [llength $keys]
    }

    test "SCAN MATCH" {
        r flushdb
        r debug populate 1000

        set cur 0
        set keys {}
        while 1 {
            set res [r scan $cur match "key:1??"]
            set cur [lindex $res 0]
            set k [lindex $res 1]
            lappend keys {*}$k
            if {$cur == 0} break
        }

        set keys [lsort -unique $keys]
        assert_equal 100 [llength $keys]
    }

    foreach enc {intset hashtable} {
        test "SSCAN with encoding $enc" {
            # Create the Set
            r del set
            if {$enc eq {intset}} {
                set prefix ""
            } else {
                set prefix "ele:"
            }
            set elements {}
            for {set j 0} {$j < 100} {incr j} {
                lappend elements ${prefix}${j}
            }
            r sadd set {*}$elements

            # Verify that the encoding matches.
            assert {[r object encoding set] eq $enc}

            # Test SSCAN
            set cur 0
            set keys {}
            while 1 {
                set res [r sscan set $cur]
                set cur [lindex $res 0]
                set k [lindex $res 1]
                lappend keys {*}$k
                if {$cur == 0} break
            }

            set keys [lsort -unique $keys]
            assert_equal 100 [llength $keys]
        }
    }

    foreach enc {ziplist hashtable} {
        test "HSCAN with encoding $enc" {
            # Create the Hash
            r del hash
            if {$enc eq {ziplist}} {
                set count 30
            } else {
                set count 1000
            }
            set elements {}
            for {set j 0} {$j < $count} {incr j} {
                lappend elements key:$j $j
            }
            r hmset hash {*}$elements

            # Verify that the encoding matches.
            assert {[r object encoding hash] eq $enc}

            # Test HSCAN
            set cur 0
            set keys {}
            while 1 {
                set res [r hscan hash $cur]
                set cur [lindex $res 0]
                set k [lindex $res 1]
                lappend keys {*}$k
                if {$cur == 0} break
            }

            set keys2 {}
            foreach {k v} $keys {
                assert {$k eq "key:$v"}
                lappend keys2 $k
            }

            set keys2 [lsort -unique $keys2]
            assert_equal $count [llength $keys2]
        }
    }

    foreach enc {ziplist skiplist} {
        test "ZSCAN with encoding $enc" {
            # Create the Sorted Set
            r del zset
            if {$enc eq {ziplist}} {
                set count 30
            } else {
                set count 1000
            }
            set elements {}
            for {set j 0} {$j < $count} {incr j} {
                lappend elements $j key:$j
            }
            r zadd zset {*}$elements

            # Verify that the encoding matches.
            assert {[r object encoding zset] eq $enc}

            # Test ZSCAN
            set cur 0
            set keys {}
            while 1 {
                set res [r zscan zset $cur]
                set cur [lindex $res 0]
                set k [lindex $res 1]
                lappend keys {*}$k
                if {$cur == 0} break
            }

            set keys2 {}
            foreach {k v} $keys {
                assert {$k eq "key:$v"}
                lappend keys2 $k
            }

            set keys2 [lsort -unique $keys2]
            assert_equal $count [llength $keys2]
        }
    }

    test "SCAN guarantees check under write load" {
        r flushdb
        r debug populate 100

        # We start scanning here, so keys from 0 to 99 should all be
        # reported at the end of the iteration.
        set keys {}
        while 1 {
            set res [r scan $cur]
            set cur [lindex $res 0]
            set k [lindex $res 1]
            lappend keys {*}$k
            if {$cur == 0} break
            # Write 10 random keys at every SCAN iteration.
            for {set j 0} {$j < 10} {incr j} {
                r set addedkey:[randomInt 1000] foo
            }
        }

        set keys2 {}
        foreach k $keys {
            if {[string length $k] > 6} continue
            lappend keys2 $k
        }

        set keys2 [lsort -unique $keys2]
        assert_equal 100 [llength $keys2]
    }

    test "SSCAN with integer encoded object (issue #1345)" {
        set objects {1 a}
        r del set
        r sadd set {*}$objects
        set res [r sscan set 0 MATCH *a* COUNT 100]
        assert_equal [lsort -unique [lindex $res 1]] {a}
        set res [r sscan set 0 MATCH *1* COUNT 100]
        assert_equal [lsort -unique [lindex $res 1]] {1}
    }

    test "SSCAN with PATTERN" {
        r del mykey
        r sadd mykey foo fab fiz foobar 1 2 3 4
        set res [r sscan mykey 0 MATCH foo* COUNT 10000]
        lsort -unique [lindex $res 1]
    } {foo foobar}

    test "HSCAN with PATTERN" {
        r del mykey
        r hmset mykey foo 1 fab 2 fiz 3 foobar 10 1 a 2 b 3 c 4 d
        set res [r hscan mykey 0 MATCH foo* COUNT 10000]
        lsort -unique [lindex $res 1]
    } {1 10 foo foobar}

    test "ZSCAN with PATTERN" {
        r del mykey
        r zadd mykey 1 foo 2 fab 3 fiz 10 foobar
        set res [r zscan mykey 0 MATCH foo* COUNT 10000]
        lsort -unique [lindex $res 1]
    }
}
