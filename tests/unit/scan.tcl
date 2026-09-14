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

    test "{$type} SCAN with expired keys with TYPE filter and PATTERN filter" {
        r flushdb
        # make sure that passive expiration is triggered by the scan
        r debug set-active-expire 0

        populate 1000
        r set key:foo bar
        r pexpire key:foo 1

        # add a hash type key
        r hset key:hash f v
        r pexpire key:hash 1

        # add a pattern key
        r set boo far
        r pexpire boo 1

        after 2

        set cur 0
        set keys {}
        while 1 {
            set res [r scan $cur type "string" match key* count 10]
            set cur [lindex $res 0]
            set k [lindex $res 1]
            lappend keys {*}$k
            if {$cur == 0} break
        }

        assert_equal 1000 [llength $keys]

        # make sure that expired key have been removed by scan command, 
        # pattern check before expired so key filtered by pattern will not be removed
        # but expiration check is before type check so key:foo and key:hash will be removed
        assert_equal 1001 [scan [regexp -inline {keys\=([\d]*)} [r info keyspace]] keys=%d]
        
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

            # Test NOVALUES 
            set res [r hscan hash 0 count 1000 novalues]
            assert_equal [lsort $keys2] [lsort [lindex $res 1]]
        }

        test "{$type} HSCAN with large value $enc" {
            r del hash

            if {$enc eq {listpack}} {
                set count 60
            } else {
                set count 170
            }

            set val1 [string repeat "1" $count]
            r hset hash $val1 $val1

            set val2 [string repeat "2" $count]
            r hset hash $val2 $val2

            set res [lsort [lindex [r hscan hash 0] 1]]
            assert_equal $val1 [lindex $res 0]
            assert_equal $val1 [lindex $res 1]
            assert_equal $val2 [lindex $res 2]
            assert_equal $val2 [lindex $res 3]

            set res [lsort [lindex [r hscan hash 0 novalues] 1]]
            assert_equal $val1 [lindex $res 0]
            assert_equal $val2 [lindex $res 1]
        }
    }

    foreach enc {listpack btree} {
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

    if {$type eq {standalone}} {
        test {B+ tree ZSCAN keeps scores as bulk strings in RESP3} {
            r del zset
            set elements {}
            for {set j 0} {$j < 129} {incr j} {
                set score [expr {$j == 0 ? 3.3 : 1.25}]
                lappend elements $score member:$j
            }
            r zadd zset {*}$elements
            assert_encoding btree zset

            r hello 3
            r readraw 1
            assert_equal {*2} [r zscan zset 0 count 10000]
            assert_equal {$1} [r read]
            assert_equal 0 [r read]
            set array_header [r read]
            assert_equal {*} [string index $array_header 0]
            set fields [string range $array_header 1 end]
            assert {$fields % 2 == 0}
            set seen {}
            for {set j 0} {$j < $fields / 2} {incr j} {
                assert_match {$*} [r read]
                set member [r read]
                set score_header [r read]
                set score [r read]
                dict set seen $member 1
                if {$member eq {member:0}} {
                    assert_equal {$18} $score_header
                    assert_equal 3.2999999999999998 $score
                } else {
                    assert_equal {$4} $score_header
                    assert_equal 1.25 $score
                }
            }
            assert_equal 129 [dict size $seen]
            r readraw 0
            r hello 2
            set _ {}
        } {} {resp3}
    }

    test "{$type} ZSCAN does not skip members after deleting returned members" {
        r del zset
        set elements {}
        for {set j 0} {$j < 1000} {incr j} {
            lappend elements $j key:$j
        }
        r zadd zset {*}$elements
        assert_encoding btree zset

        lassign [r zscan zset 0 count 10] cursor items
        set seen {}
        foreach {member score} $items {
            lappend seen $member
            r zrem zset $member
        }
        while {$cursor != 0} {
            lassign [r zscan zset $cursor count 10] cursor items
            foreach {member score} $items {
                lappend seen $member
            }
        }

        assert_equal 1000 [llength [lsort -unique $seen]]
    }

    test "{$type} ZSCAN does not skip members changed between calls" {
        r del zset
        set elements {}
        set members {}
        for {set j 0} {$j < 2048} {incr j} {
            set member [format "stable:%05d" $j]
            lappend elements $j $member
            lappend members $member
        }
        r zadd zset {*}$elements
        assert_encoding btree zset

        set cursor 0
        set seen {}
        set calls 0
        while 1 {
            lassign [r zscan zset $cursor count 1] cursor items
            foreach {member score} $items {
                lappend seen $member
            }
            if {$cursor == 0} break
            incr calls

            # Moving a member can change its score leaf, but it remains part of
            # the set for the complete scan and must eventually be returned.
            set member [lindex $members [expr {($calls * 31) % 2048}]]
            r zadd zset [expr {2048 + $calls}] $member
        }

        assert_equal 2048 [llength [lsort -unique $seen]]
    }

    test "{$type} ZSCAN does not skip members while its table grows" {
        r del zset
        set elements {}
        for {set j 0} {$j < 1984} {incr j} {
            lappend elements $j [format "stable:%05d" $j]
        }
        r zadd zset {*}$elements
        assert_encoding btree zset

        set cursor 0
        set seen {}
        set calls 0
        while 1 {
            lassign [r zscan zset $cursor count 1] cursor items
            foreach {member score} $items {
                if {[string match "stable:*" $member]} {
                    lappend seen $member
                }
            }
            if {$cursor == 0} break

            # A 256-bucket narrow table has 2048 slots and grows when the
            # 1985th member is added. Further additions advance the copy.
            incr calls
            r zadd zset [expr {1984 + $calls}] [format "added:%05d" $calls]
        }

        assert_equal 1984 [llength [lsort -unique $seen]]
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

    test "{$type} HSCAN with NOVALUES" {
        r del mykey
        r hmset mykey foo 1 fab 2 fiz 3 foobar 10 1 a 2 b 3 c 4 d
        set res [r hscan mykey 0 NOVALUES]
        lsort -unique [lindex $res 1]
    } {1 2 3 4 fab fiz foo foobar}

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

    test "{$type} SCAN COUNT overflow" {
        r flushdb
        populate 10

        # count = LONG_MAX/10 + 1, within LONG_MAX so it parses fine,
        # but count*10 overflows signed long which is undefined behavior.
        # Compute dynamically to support both 32-bit and 64-bit builds.
        set long_max [expr {[s arch_bits] == 32 ? 2147483647 : 9223372036854775807}]
        set big_count [expr {$long_max / 10 + 1}]
        set res [r scan 0 count $big_count]
        assert {[llength $res] == 2}
        assert_equal 0 [lindex $res 0]
        assert_equal 10 [llength [lindex $res 1]]
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
