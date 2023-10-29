proc test_scan {type} {
    test "{$type} SCAN basic" {
        r flushdb
        populate 1000

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

   test "{$type} SCAN COUNT" {
        r flushdb
        populate 1000

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

    test "{$type} SCAN MATCH" {
        r flushdb
        populate 1000

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

    test "{$type} SCAN TYPE" {
        r flushdb
        # populate only creates strings
        populate 1000

        # Check non-strings are excluded
        set cur 0
        set keys {}
        while 1 {
            set res [r scan $cur type "list"]
            set cur [lindex $res 0]
            set k [lindex $res 1]
            lappend keys {*}$k
            if {$cur == 0} break
        }

        assert_equal 0 [llength $keys]

        # Check strings are included
        set cur 0
        set keys {}
        while 1 {
            set res [r scan $cur type "string"]
            set cur [lindex $res 0]
            set k [lindex $res 1]
            lappend keys {*}$k
            if {$cur == 0} break
        }

        assert_equal 1000 [llength $keys]

        # Check all three args work together
        set cur 0
        set keys {}
        while 1 {
            set res [r scan $cur type "string" match "key:*" count 10]
            set cur [lindex $res 0]
            set k [lindex $res 1]
            lappend keys {*}$k
            if {$cur == 0} break
        }

        assert_equal 1000 [llength $keys]
    }

    test "{$type} SCAN unknown type" {
        r flushdb
        # make sure that passive expiration is triggered by the scan
        r debug set-active-expire 0

        populate 1000
        r hset hash f v
        r pexpire hash 1

        after 2

        # TODO: remove this in redis 8.0
        set cur 0
        set keys {}
        while 1 {
            set res [r scan $cur type "string1"]
            set cur [lindex $res 0]
            set k [lindex $res 1]
            lappend keys {*}$k
            if {$cur == 0} break
        }

        assert_equal 0 [llength $keys]
        # make sure that expired key have been removed by scan command
        assert_equal 1000 [scan [regexp -inline {keys\=([\d]*)} [r info keyspace]] keys=%d]

        # TODO: uncomment in redis 8.0
        #assert_error "*unknown type name*" {r scan 0 type "string1"}
        # expired key will be no touched by scan command
        #assert_equal 1001 [scan [regexp -inline {keys\=([\d]*)} [r info keyspace]] keys=%d]
        r debug set-active-expire 1
    } {OK} {needs:debug}

    test "{$type} SCAN with expired keys" {
        r flushdb
        # make sure that passive expiration is triggered by the scan
        r debug set-active-expire 0

        populate 1000
        r set foo bar
        r pexpire foo 1
        
        # add a hash type key
        r hset hash f v
        r pexpire hash 1
        
        after 2

        set cur 0
        set keys {}
        while 1 {
            set res [r scan $cur count 10]
            set cur [lindex $res 0]
            set k [lindex $res 1]
            lappend keys {*}$k
            if {$cur == 0} break
        }

        assert_equal 1000 [llength $keys]

        # make sure that expired key have been removed by scan command
        assert_equal 1000 [scan [regexp -inline {keys\=([\d]*)} [r info keyspace]] keys=%d]

        r debug set-active-expire 1
    } {OK} {needs:debug}

    test "{$type} SCAN with expired keys with TYPE filter" {
        r flushdb
        # make sure that passive expiration is triggered by the scan
        r debug set-active-expire 0

        populate 1000
        r set foo bar
        r pexpire foo 1

        # add a hash type key
        r hset hash f v
        r pexpire hash 1

        after 2

        set cur 0
        set keys {}
        while 1 {
            set res [r scan $cur type "string" count 10]
            set cur [lindex $res 0]
            set k [lindex $res 1]
            lappend keys {*}$k
            if {$cur == 0} break
        }

        assert_equal 1000 [llength $keys]

        # make sure that expired key have been removed by scan command
        assert_equal 1000 [scan [regexp -inline {keys\=([\d]*)} [r info keyspace]] keys=%d]
        # TODO: uncomment in redis 8.0
        # make sure that only the expired key in the type match will been removed by scan command
        #assert_equal 1001 [scan [regexp -inline {keys\=([\d]*)} [r info keyspace]] keys=%d]

        r debug set-active-expire 1
    } {OK} {needs:debug}

    foreach enc {intset listpack hashtable} {
        test "{$type} SSCAN with encoding $enc" {
            # Create the Set
            r del set
            if {$enc eq {intset}} {
                set prefix ""
            } else {
                set prefix "ele:"
            }
            set count [expr {$enc eq "hashtable" ? 200 : 100}]
            set elements {}
            for {set j 0} {$j < $count} {incr j} {
                lappend elements ${prefix}${j}
            }
            r sadd set {*}$elements

            # Verify that the encoding matches.
            assert_encoding $enc set

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
            assert_equal $count [llength $keys]
        }
    }

    foreach enc {listpack hashtable} {
        test "{$type} HSCAN with encoding $enc" {
            # Create the Hash
            r del hash
            if {$enc eq {listpack}} {
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
            assert_encoding $enc hash

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

    foreach enc {listpack skiplist} {
        test "{$type} ZSCAN with encoding $enc" {
            # Create the Sorted Set
            r del zset
            if {$enc eq {listpack}} {
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
            assert_encoding $enc zset

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

    test "{$type} SCAN guarantees check under write load" {
        r flushdb
        populate 100

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

    test "{$type} SSCAN with integer encoded object (issue #1345)" {
        set objects {1 a}
        r del set
        r sadd set {*}$objects
        set res [r sscan set 0 MATCH *a* COUNT 100]
        assert_equal [lsort -unique [lindex $res 1]] {a}
        set res [r sscan set 0 MATCH *1* COUNT 100]
        assert_equal [lsort -unique [lindex $res 1]] {1}
    }

    test "{$type} SSCAN with PATTERN" {
        r del mykey
        r sadd mykey foo fab fiz foobar 1 2 3 4
        set res [r sscan mykey 0 MATCH foo* COUNT 10000]
        lsort -unique [lindex $res 1]
    } {foo foobar}

    test "{$type} HSCAN with PATTERN" {
        r del mykey
        r hmset mykey foo 1 fab 2 fiz 3 foobar 10 1 a 2 b 3 c 4 d
        set res [r hscan mykey 0 MATCH foo* COUNT 10000]
        lsort -unique [lindex $res 1]
    } {1 10 foo foobar}

    test "{$type} ZSCAN with PATTERN" {
        r del mykey
        r zadd mykey 1 foo 2 fab 3 fiz 10 foobar
        set res [r zscan mykey 0 MATCH foo* COUNT 10000]
        lsort -unique [lindex $res 1]
    }

    test "{$type} ZSCAN scores: regression test for issue #2175" {
        r del mykey
        for {set j 0} {$j < 500} {incr j} {
            r zadd mykey 9.8813129168249309e-323 $j
        }
        set res [lindex [r zscan mykey 0] 1]
        set first_score [lindex $res 1]
        assert {$first_score != 0}
    }

    test "{$type} SCAN regression test for issue #4906" {
        for {set k 0} {$k < 100} {incr k} {
            r del set
            r sadd set x; # Make sure it's not intset encoded
            set toremove {}
            unset -nocomplain found
            array set found {}

            # Populate the set
            set numele [expr {101+[randomInt 1000]}]
            for {set j 0} {$j < $numele} {incr j} {
                r sadd set $j
                if {$j >= 100} {
                    lappend toremove $j
                }
            }

            # Start scanning
            set cursor 0
            set iteration 0
            set del_iteration [randomInt 10]
            while {!($cursor == 0 && $iteration != 0)} {
                lassign [r sscan set $cursor] cursor items

                # Mark found items. We expect to find from 0 to 99 at the end
                # since those elements will never be removed during the scanning.
                foreach i $items {
                    set found($i) 1
                }
                incr iteration
                # At some point remove most of the items to trigger the
                # rehashing to a smaller hash table.
                if {$iteration == $del_iteration} {
                    r srem set {*}$toremove
                }
            }

            # Verify that SSCAN reported everything from 0 to 99
            for {set j 0} {$j < 100} {incr j} {
                if {![info exists found($j)]} {
                    fail "SSCAN element missing $j"
                }
            }
        }
    }

    test "{$type} SCAN MATCH pattern implies cluster slot" {
        # Tests the code path for an optimization for patterns like "{foo}-*"
        # which implies that all matching keys belong to one slot.
        r flushdb
        for {set j 0} {$j < 100} {incr j} {
            r set "{foo}-$j" "foo"; # slot 12182
            r set "{bar}-$j" "bar"; # slot 5061
            r set "{boo}-$j" "boo"; # slot 13142
        }

        set cursor 0
        set keys {}
        while 1 {
            set res [r scan $cursor match "{foo}-*"]
            set cursor [lindex $res 0]
            set k [lindex $res 1]
            lappend keys {*}$k
            if {$cursor == 0} break
        }

        set keys [lsort -unique $keys]
        assert_equal 100 [llength $keys]
    }
}

start_server {tags {"scan network standalone"}} {
    test_scan "standalone"
}

start_cluster 1 0 {tags {"external:skip cluster scan"}} {
    test_scan "cluster"
}
