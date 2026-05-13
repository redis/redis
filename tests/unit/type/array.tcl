start_server {
    tags {"array"}
} {
    # Basic ARSET/ARGET tests
    test {ARSET and ARGET basics} {
        r del myarray
        assert_equal 1 [r arset myarray 0 hello]
        assert_equal hello [r arget myarray 0]
        assert_equal {} [r arget myarray 1]
    }

    test {ARSET overwrites existing value} {
        r del myarray
        assert_equal 1 [r arset myarray 0 hello]
        assert_equal 0 [r arset myarray 0 world]
        assert_equal world [r arget myarray 0]
    }

    test {ARGET non-existing key} {
        r del myarray
        assert_equal {} [r arget myarray 0]
    }

    test {ARGET validates index even on non-existing key} {
        r del myarray
        assert_error {*invalid array index*} {r arget myarray not-an-index}
    }

    test {ARSET/ARGET with integer values} {
        r del myarray
        r arset myarray 0 12345
        assert_equal 12345 [r arget myarray 0]
    }

    test {ARSET/ARGET with float values} {
        r del myarray
        r arset myarray 0 3.14159
        assert_equal 3.14159 [r arget myarray 0]
    }

    test {ARSET/ARGET with small strings} {
        r del myarray
        r arset myarray 0 abc
        assert_equal abc [r arget myarray 0]
    }

    test {ARSET/ARGET with large string} {
        r del myarray
        set longstr [string repeat x 100]
        r arset myarray 0 $longstr
        assert_equal $longstr [r arget myarray 0]
    }

    test {ARSET/ARGET with empty string} {
        r del myarray
        r arset myarray 0 ""
        assert_equal "" [r arget myarray 0]
    }

    # ARLEN and ARCOUNT tests
    test {ARLEN and ARCOUNT basics} {
        r del myarray
        assert_equal 0 [r arlen myarray]
        assert_equal 0 [r arcount myarray]

        r arset myarray 0 a
        assert_equal 1 [r arlen myarray]
        assert_equal 1 [r arcount myarray]

        r arset myarray 5 b
        assert_equal 6 [r arlen myarray]
        assert_equal 2 [r arcount myarray]

        r arset myarray 100 c
        assert_equal 101 [r arlen myarray]
        assert_equal 3 [r arcount myarray]
    }

    # ARDEL tests
    test {ARDEL basics} {
        r del myarray
        r arset myarray 0 a
        r arset myarray 1 b
        r arset myarray 2 c

        assert_equal 1 [r ardel myarray 1]
        assert_equal {} [r arget myarray 1]
        assert_equal 2 [r arcount myarray]

        # Delete non-existing index returns 0
        assert_equal 0 [r ardel myarray 1]
    }

    test {ARDEL multiple indices} {
        r del myarray
        r arset myarray 0 a
        r arset myarray 1 b
        r arset myarray 2 c
        r arset myarray 3 d

        assert_equal 3 [r ardel myarray 0 1 2]
        assert_equal 1 [r arcount myarray]
    }

    test {ARDEL last element deletes key} {
        r del myarray
        r arset myarray 0 a
        r ardel myarray 0
        assert_equal 0 [r exists myarray]
    }

    test {ARDEL notifies array event before del when key is removed} {
        set orig_notify [lindex [r config get notify-keyspace-events] 1]
        r config set notify-keyspace-events KEA
        r del myarray
        r arset myarray 0 a

        set rd1 [redis_deferring_client]
        assert_equal {1} [psubscribe $rd1 *]
        assert_equal 1 [r ardel myarray 0]

        assert_match "pmessage * __keyspace@*__:myarray ardel" [$rd1 read]
        assert_match "pmessage * __keyevent@*__:ardel myarray" [$rd1 read]
        assert_match "pmessage * __keyspace@*__:myarray del" [$rd1 read]
        assert_match "pmessage * __keyevent@*__:del myarray" [$rd1 read]

        $rd1 close
        r config set notify-keyspace-events $orig_notify
    }

    # ARDELRANGE tests
    test {ARDELRANGE basics} {
        r del myarray
        for {set i 0} {$i < 10} {incr i} {
            r arset myarray $i [expr $i * 10]
        }
        assert_equal 10 [r arcount myarray]

        assert_equal 5 [r ardelrange myarray 2 6]
        assert_equal 5 [r arcount myarray]
    }

    test {ARDELRANGE reverse order} {
        r del myarray
        for {set i 0} {$i < 10} {incr i} {
            r arset myarray $i [expr $i * 10]
        }

        assert_equal 5 [r ardelrange myarray 6 2]
        assert_equal 5 [r arcount myarray]
    }

    test {ARDELRANGE notifies array event before del when key is removed} {
        set orig_notify [lindex [r config get notify-keyspace-events] 1]
        r config set notify-keyspace-events KEA
        r del myarray
        assert_equal 3 [r arset myarray 0 a b c]

        set rd1 [redis_deferring_client]
        assert_equal {1} [psubscribe $rd1 *]
        assert_equal 3 [r ardelrange myarray 0 2]

        assert_match "pmessage * __keyspace@*__:myarray ardelrange" [$rd1 read]
        assert_match "pmessage * __keyevent@*__:ardelrange myarray" [$rd1 read]
        assert_match "pmessage * __keyspace@*__:myarray del" [$rd1 read]
        assert_match "pmessage * __keyevent@*__:del myarray" [$rd1 read]

        $rd1 close
        r config set notify-keyspace-events $orig_notify
    }

    # ARMSET and ARMGET tests
    test {ARMSET basics} {
        r del myarray
        assert_equal 3 [r armset myarray 0 a 1 b 2 c]
        assert_equal a [r arget myarray 0]
        assert_equal b [r arget myarray 1]
        assert_equal c [r arget myarray 2]
    }

    test {ARMSET returns only newly filled slots} {
        r del myarray
        r arset myarray 0 a
        assert_equal 1 [r armset myarray 0 aa 1 b]
        assert_equal aa [r arget myarray 0]
        assert_equal b [r arget myarray 1]
    }

    test {ARMGET basics} {
        r del myarray
        r arset myarray 0 a
        r arset myarray 1 b
        r arset myarray 5 c

        set result [r armget myarray 0 1 5 3]
        assert_equal a [lindex $result 0]
        assert_equal b [lindex $result 1]
        assert_equal c [lindex $result 2]
        assert_equal {} [lindex $result 3]
    }

    # ARGETRANGE and contiguous ARSET tests
    test {ARGETRANGE basics} {
        r del myarray
        r armset myarray 0 a 1 b 2 c 3 d 4 e

        set result [r argetrange myarray 1 3]
        assert_equal {b c d} $result
    }

    test {ARGETRANGE reverse} {
        r del myarray
        r armset myarray 0 a 1 b 2 c 3 d 4 e

        set result [r argetrange myarray 3 1]
        assert_equal {d c b} $result
    }

    test {ARGETRANGE errors when requested range exceeds the hard limit} {
        assert_error {*range exceeds maximum of 1000000 items*} {r argetrange myarray 0 1000000}
    }

    test {ARGETRANGE reverse errors when requested range exceeds the hard limit} {
        assert_error {*range exceeds maximum of 1000000 items*} {r argetrange myarray 1000000 0}
    }

    # ARSCAN tests
    test {ARSCAN returns only existing elements with indices} {
        r del myarray
        r arset myarray 0 a
        r arset myarray 5 b
        r arset myarray 9 c

        set result [r arscan myarray 0 10]
        assert_equal {{0 a} {5 b} {9 c}} $result
    }

    test {ARSCAN on empty range returns empty array} {
        r del myarray
        r arset myarray 500 x

        set result [r arscan myarray 0 100]
        assert_equal {} $result
    }

    test {ARSCAN reversed range} {
        r del myarray
        r arset myarray 0 a
        r arset myarray 5 b

        set result [r arscan myarray 5 0]
        assert_equal {{5 b} {0 a}} $result
    }

    test {ARSCAN on non-existent key returns empty array} {
        r del nokey
        set result [r arscan nokey 0 100]
        assert_equal {} $result
    }

    test {ARSCAN with mixed value types} {
        r del myarray
        r arset myarray 0 string
        r arset myarray 1 12345
        r arset myarray 2 3.14

        set result [r arscan myarray 0 10]
        assert_equal 3 [llength $result]
        assert_equal {0 string} [lindex $result 0]
        assert_equal {1 12345} [lindex $result 1]
        assert_equal {2 3.14} [lindex $result 2]
    }

    # ARGREP tests
    test {ARGREP MATCH returns matching indexes} {
        r del myarray
        r armset myarray 0 alpha 1 beta 2 alphabet 5 gamma

        assert_equal {0 2} [r argrep myarray - + MATCH alpha]
    }

    test {ARGREP supports WITHVALUES and reverse ranges} {
        r del myarray
        r armset myarray 0 alpha 1 beta 2 alphabet 3 delta

        assert_equal {{2 alphabet} {0 alpha}} \
            [r argrep myarray 3 0 MATCH alpha WITHVALUES]
    }

    test {ARGREP supports AND, GLOB, and NOCASE} {
        r del myarray
        r armset myarray 0 RedisArray 1 redis-match 2 array-only 3 plain

        assert_equal {0} [r argrep myarray - + MATCH redis GLOB *array* AND NOCASE]
    }

    test {ARGREP supports RE predicates} {
        r del myarray
        r armset myarray 0 foo123 1 bar 2 zoo999 3 Foo777

        assert_equal {0 2 3} [r argrep myarray - + RE {^.*[0-9]{3}$}]
        assert_equal {0 3} [r argrep myarray - + RE {^foo[0-9]+$} NOCASE]
    }

    test {ARGREP RE literal alternation forms still match correctly} {
        r del myarray
        r armset myarray 0 foo 1 bar 2 baz 3 foobar 4 BAR 5 quxfoo 6 zedbar \
            7 plain 8 ALPS 9 alphabet

        assert_equal {0 1 3 5 6} [r argrep myarray - + RE {foo|bar}]
        assert_equal {0 1 3 4 5 6} [r argrep myarray - + RE {foo|bar} NOCASE]
        assert_equal {0 1 4} [r argrep myarray - + RE {^(foo|bar)$} NOCASE]
        assert_equal {0 1 3 4} [r argrep myarray - + RE {^(foo|bar)} NOCASE]
        assert_equal {0 1 3 4 5 6} [r argrep myarray - + RE {(foo|bar)$} NOCASE]
        assert_equal {8 9} [r argrep myarray - + RE {alpha|alps} NOCASE]
    }

    test {ARGREP RE grouped alternation smoke test} {
        r del myarray
        r armset myarray 0 item-foo-123 1 ITEM-BAR-456 2 item-baz 3 plain

        assert_equal {0 1} \
            [r argrep myarray - + RE {^item-(foo|bar)-[0-9]{3}$} NOCASE]
    }

    test {ARGREP enforces RE length and rejects backreferences} {
        r del myarray
        set re2048 [string repeat a 2048]
        set re2049 [string repeat a 2049]
        r arset myarray 0 $re2048

        assert_equal {0} [r argrep myarray - + RE $re2048]
        assert_error {*maximum is 2048 bytes*} {r argrep myarray - + RE $re2049}
        assert_error {*backreferences are not supported*} {r argrep myarray - + RE {(a)\1}}
        assert_error {*regular expression is empty*} {r argrep myarray - + RE {}}
    }

    test {ARGREP LIMIT stops after enough matches} {
        r del myarray
        r armset myarray 0 hit-1 1 hit-2 2 miss 3 hit-3

        assert_equal {0 1} [r argrep myarray - + MATCH hit LIMIT 2]
    }

    test {ARGREP allows mixed predicate and option order, last wins} {
        r del myarray
        r armset myarray 0 RedisArray 1 redis-match 2 array-only 3 plain

        assert_equal {0} \
            [r argrep myarray - + OR MATCH redis LIMIT 3 GLOB *array* AND LIMIT 1 NOCASE]
    }

    test {ARGREP enforces the predicate limit} {
        r del myarray
        r arset myarray 0 foo

        set cmd [list r argrep myarray - +]
        for {set i 0} {$i < 250} {incr i} {
            lappend cmd MATCH foo
        }
        assert_equal {0} [uplevel 1 $cmd]

        lappend cmd MATCH foo
        assert_error {*maximum is 250*} [list uplevel 1 $cmd]
    }

    test {ARGREP handles missing keys and syntax errors} {
        r del nokey
        assert_equal {} [r argrep nokey - + MATCH foo]
        assert_error {*syntax error*} {r argrep myarray - + LIMIT 1}
        assert_error {*invalid regular expression*} {r argrep myarray - + RE {(}}
    }

    test {ARGREP rejects malformed braced hex regex escapes} {
        r del myarray
        r arset myarray 0 hello

        set invalid [format "\\%c%c1" 120 123]
        assert_error {*invalid regular expression*} [list r argrep myarray - + RE $invalid]
        assert_error {*invalid regular expression*} [list r argrep myarray - + RE $invalid NOCASE]
    }

    test {ARSET contiguous write basics} {
        r del myarray
        assert_equal 3 [r arset myarray 0 a b c]
        assert_equal a [r arget myarray 0]
        assert_equal b [r arget myarray 1]
        assert_equal c [r arget myarray 2]
    }

    # ARINSERT tests
    test {ARINSERT basics} {
        r del myarray
        assert_equal 0 [r arinsert myarray a]
        assert_equal 1 [r arinsert myarray b]
        assert_equal 2 [r arinsert myarray c]

        assert_equal a [r arget myarray 0]
        assert_equal b [r arget myarray 1]
        assert_equal c [r arget myarray 2]
    }

    test {ARRING creates ring buffer} {
        r del myarray
        for {set i 0} {$i < 10} {incr i} {
            r arring myarray 5 $i
        }

        # After wrap, we should have indices 0-4 with values 5-9
        assert_equal 5 [r arget myarray 0]
        assert_equal 6 [r arget myarray 1]
        assert_equal 7 [r arget myarray 2]
        assert_equal 8 [r arget myarray 3]
        assert_equal 9 [r arget myarray 4]
        assert_equal 5 [r arcount myarray]
    }

    # ARNEXT, ARSEEK tests
    test {ARNEXT tracks insert position} {
        r del myarray
        assert_equal 0 [r arnext myarray]

        r arinsert myarray a
        assert_equal 1 [r arnext myarray]

        r arinsert myarray b
        assert_equal 2 [r arnext myarray]
    }

    test {ARSEEK} {
        r del myarray
        r arinsert myarray a
        r arinsert myarray b

        assert_equal 1 [r arseek myarray 10]
        r arinsert myarray c
        assert_equal 11 [r arnext myarray]
        assert_equal c [r arget myarray 10]
    }

    test {ARNEXT returns null when insert cursor is exhausted} {
        r del myarray
        r arinsert myarray a

        # Move to terminal cursor state: insert_idx = UINT64_MAX-1
        r arseek myarray 18446744073709551615
        assert_equal {} [r arnext myarray]
        assert_error {*insert index overflow*} {r arinsert myarray b}
    }

    # ARLASTITEMS tests
    test {ARLASTITEMS basics} {
        r del myarray
        for {set i 0} {$i < 5} {incr i} {
            r arinsert myarray [expr $i * 10]
        }

        set result [r arlastitems myarray 3]
        assert_equal {20 30 40} $result

        set result [r arlastitems myarray 3 REV]
        assert_equal {40 30 20} $result
    }

    test {ARLASTITEMS after ARSEEK 0 uses array tail} {
        r del myarray
        for {set i 0} {$i < 5} {incr i} {
            r arinsert myarray [expr $i * 10]
        }

        assert_equal 1 [r arseek myarray 0]
        assert_equal {20 30 40} [r arlastitems myarray 3]
        assert_equal {40 30 20} [r arlastitems myarray 3 REV]
    }

    # AROP tests
    test {AROP SUM} {
        r del myarray
        r armset myarray 0 10 1 20 2 30

        set result [r arop myarray 0 2 SUM]
        assert_equal 60 $result
    }

    test {AROP MIN} {
        r del myarray
        r armset myarray 0 30 1 10 2 20

        set result [r arop myarray 0 2 MIN]
        assert_equal 10 $result
    }

    test {AROP MAX} {
        r del myarray
        r armset myarray 0 30 1 10 2 20

        set result [r arop myarray 0 2 MAX]
        assert_equal 30 $result
    }

    test {AROP MATCH} {
        r del myarray
        r armset myarray 0 hello 1 world 2 hello 3 foo

        assert_equal 2 [r arop myarray 0 3 MATCH hello]
        assert_equal 1 [r arop myarray 0 3 MATCH world]
        assert_equal 0 [r arop myarray 0 3 MATCH bar]
    }

    test {AROP USED} {
        r del myarray
        r armset myarray 0 a 2 b 5 c

        assert_equal 3 [r arop myarray 0 10 USED]
    }

    test {AROP AND/OR/XOR} {
        r del myarray
        # Use decimal values: 255, 15, 240
        r armset myarray 0 255 1 15 2 240

        assert_equal 0 [r arop myarray 0 2 AND]
        assert_equal 255 [r arop myarray 0 2 OR]
        assert_equal 0 [r arop myarray 0 2 XOR]
    }

    test {AROP AND/OR/XOR truncates floats toward zero} {
        r del myarray
        # Truncated values: 7, 3, 1
        r armset myarray 0 7.9 1 3.2 2 1.8

        assert_equal 1 [r arop myarray 0 2 AND]
        assert_equal 7 [r arop myarray 0 2 OR]
        assert_equal 5 [r arop myarray 0 2 XOR]
    }

    # ARINFO tests
    test {ARINFO basics} {
        r del myarray
        r armset myarray 0 a 1 b 100 c

        set info [r arinfo myarray]
        assert_equal 3 [dict get $info count]
        assert_equal 101 [dict get $info len]
    }

    # Type check tests
    test {Array commands on wrong type} {
        r del mykey
        r set mykey value
        assert_error {WRONGTYPE*} {r arget mykey 0}
        assert_error {WRONGTYPE*} {r arset mykey 0 foo}
        assert_error {WRONGTYPE*} {r arlen mykey}
        assert_error {WRONGTYPE*} {r arcount mykey}
    }

    # TYPE command
    test {TYPE returns array} {
        r del myarray
        r arset myarray 0 hello
        assert_equal array [r type myarray]
    }

    # OBJECT ENCODING command
    test {OBJECT ENCODING returns sliced-array} {
        r del myarray
        r arset myarray 0 hello
        assert_equal sliced-array [r object encoding myarray]
    }

    # Sparse indices test
    test {Sparse array with large gaps} {
        r del myarray
        r arset myarray 0 a
        r arset myarray 10000 b
        r arset myarray 1000000 c

        assert_equal a [r arget myarray 0]
        assert_equal b [r arget myarray 10000]
        assert_equal c [r arget myarray 1000000]
        assert_equal 3 [r arcount myarray]
        assert_equal 1000001 [r arlen myarray]
    }

    # RDB persistence test
    test {Array survives RDB save and load} {
        r del myarray
        r armset myarray 0 hello 1 world 100 test
        r arseek myarray 101
        r arinsert myarray value

        r bgsave
        waitForBgsave r

        r debug reload
        assert_equal hello [r arget myarray 0]
        assert_equal world [r arget myarray 1]
        assert_equal test [r arget myarray 100]
        assert_equal value [r arget myarray 101]
        assert_equal 102 [r arnext myarray]
    } {} {needs:debug}

    # =========================================================================
    # Edge case tests: directory resizing, slice transitions, window growth
    # =========================================================================

    # Directory resizing tests
    test {Directory resize - many slices} {
        r del myarray
        # Default slice size is 4096, so indices 0, 4096, 8192, 12288, etc.
        # create new slices requiring directory growth
        set slice_size 4096
        for {set i 0} {$i < 20} {incr i} {
            set idx [expr {$i * $slice_size}]
            r arset myarray $idx "slice$i"
        }

        # Verify all values
        for {set i 0} {$i < 20} {incr i} {
            set idx [expr {$i * $slice_size}]
            assert_equal "slice$i" [r arget myarray $idx]
        }
        assert_equal 20 [r arcount myarray]
    }

    test {Directory resize - very large index jump} {
        r del myarray
        r arset myarray 0 "start"
        # Jump to a very high slice index, forcing directory allocation
        r arset myarray 1000000 "middle"
        r arset myarray 10000000 "end"

        assert_equal "start" [r arget myarray 0]
        assert_equal "middle" [r arget myarray 1000000]
        assert_equal "end" [r arget myarray 10000000]
        assert_equal 3 [r arcount myarray]
    }

    # Dense slice window growth tests
    test {Dense window growth - right expansion} {
        r del myarray
        # Start with element at offset 0, then add elements going right
        # Initial window is small (8 elements), this forces growth
        for {set i 0} {$i < 100} {incr i} {
            r arset myarray $i "val$i"
        }

        # Verify all values stored correctly
        for {set i 0} {$i < 100} {incr i} {
            assert_equal "val$i" [r arget myarray $i]
        }
        assert_equal 100 [r arcount myarray]

        # Verify window grew (avg-dense-size should be >= 128 to fit 100 elements)
        set info [r arinfo myarray FULL]
        assert_equal 1 [dict get $info dense-slices]
        assert {[dict get $info avg-dense-size] >= 128}
    }

    test {Dense window growth - left expansion} {
        r del myarray
        # Start with element at high offset, then add elements going left
        # This forces window to expand leftward
        r arset myarray 500 "anchor"
        for {set i 499} {$i >= 400} {incr i -1} {
            r arset myarray $i "val$i"
        }

        assert_equal "anchor" [r arget myarray 500]
        for {set i 400} {$i < 500} {incr i} {
            assert_equal "val$i" [r arget myarray $i]
        }
        assert_equal 101 [r arcount myarray]

        # Verify window grew (avg-dense-size should be >= 128 to fit 101 elements)
        set info [r arinfo myarray FULL]
        assert_equal 1 [dict get $info dense-slices]
        assert {[dict get $info avg-dense-size] >= 128}
    }

    test {Dense window growth - bidirectional expansion} {
        r del myarray
        # Start in middle, expand both directions
        r arset myarray 500 "center"
        for {set i 1} {$i <= 50} {incr i} {
            r arset myarray [expr {500 - $i}] "left$i"
            r arset myarray [expr {500 + $i}] "right$i"
        }

        assert_equal "center" [r arget myarray 500]
        for {set i 1} {$i <= 50} {incr i} {
            assert_equal "left$i" [r arget myarray [expr {500 - $i}]]
            assert_equal "right$i" [r arget myarray [expr {500 + $i}]]
        }
        assert_equal 101 [r arcount myarray]

        # Verify window grew (avg-dense-size should be >= 128 to fit 101 elements)
        set info [r arinfo myarray FULL]
        assert_equal 1 [dict get $info dense-slices]
        assert {[dict get $info avg-dense-size] >= 128}
    }

    # Sparse to dense promotion tests
    test {Sparse to dense promotion - exceed kmax threshold} {
        r del myarray
        # kmax default is 10, add 11+ elements to force promotion
        # Use sparse pattern (scattered offsets within one slice)
        for {set i 0} {$i < 15} {incr i} {
            # Scattered within first slice (0-4095)
            set idx [expr {$i * 100}]
            r arset myarray $idx "sparse$i"
        }

        # Verify all values after promotion
        for {set i 0} {$i < 15} {incr i} {
            set idx [expr {$i * 100}]
            assert_equal "sparse$i" [r arget myarray $idx]
        }
        assert_equal 15 [r arcount myarray]

        # Verify promotion actually happened using ARINFO FULL
        set info [r arinfo myarray FULL]
        assert_equal 1 [dict get $info dense-slices]
        assert_equal 0 [dict get $info sparse-slices]
    }

    test {Sparse to dense promotion - then continue adding} {
        r del myarray
        # First create sparse slice, then promote, then add more
        for {set i 0} {$i < 5} {incr i} {
            r arset myarray [expr {$i * 200}] "phase1_$i"
        }

        # Verify starts as sparse
        set info [r arinfo myarray FULL]
        assert_equal 0 [dict get $info dense-slices]
        assert_equal 1 [dict get $info sparse-slices]

        # Add more to trigger promotion
        for {set i 5} {$i < 20} {incr i} {
            r arset myarray [expr {$i * 200}] "phase2_$i"
        }

        # Verify all
        for {set i 0} {$i < 20} {incr i} {
            assert_equal "phase[expr {$i < 5 ? 1 : 2}]_$i" [r arget myarray [expr {$i * 200}]]
        }

        # Verify promotion happened
        set info [r arinfo myarray FULL]
        assert_equal 1 [dict get $info dense-slices]
        assert_equal 0 [dict get $info sparse-slices]
    }

    # Dense to sparse demotion tests
    test {Dense to sparse demotion - delete below kmin threshold} {
        r del myarray
        # Create dense slice with many elements
        for {set i 0} {$i < 50} {incr i} {
            r arset myarray $i "val$i"
        }
        assert_equal 50 [r arcount myarray]

        # Verify starts as dense
        set info [r arinfo myarray FULL]
        assert_equal 1 [dict get $info dense-slices]
        assert_equal 0 [dict get $info sparse-slices]

        # Delete most elements, leaving only 3 (below kmin=5)
        for {set i 3} {$i < 50} {incr i} {
            r ardel myarray $i
        }

        # Verify remaining elements
        assert_equal "val0" [r arget myarray 0]
        assert_equal "val1" [r arget myarray 1]
        assert_equal "val2" [r arget myarray 2]
        assert_equal 3 [r arcount myarray]

        # Verify demotion happened
        set info [r arinfo myarray FULL]
        assert_equal 0 [dict get $info dense-slices]
        assert_equal 1 [dict get $info sparse-slices]
    }

    test {Dense to sparse demotion - then add again} {
        r del myarray
        # Create dense, demote to sparse, then add more
        for {set i 0} {$i < 30} {incr i} {
            r arset myarray $i "initial$i"
        }

        # Delete to demote
        for {set i 4} {$i < 30} {incr i} {
            r ardel myarray $i
        }
        assert_equal 4 [r arcount myarray]

        # Verify demotion happened
        set info [r arinfo myarray FULL]
        assert_equal 0 [dict get $info dense-slices]
        assert_equal 1 [dict get $info sparse-slices]

        # Add new elements (should work in sparse mode)
        for {set i 100} {$i < 105} {incr i} {
            r arset myarray $i "new$i"
        }

        # Verify old and new
        for {set i 0} {$i < 4} {incr i} {
            assert_equal "initial$i" [r arget myarray $i]
        }
        for {set i 100} {$i < 105} {incr i} {
            assert_equal "new$i" [r arget myarray $i]
        }
    }

    # Combined stress test
    test {Stress test - mixed operations across multiple slices} {
        r del myarray
        set slice_size 4096

        # Create elements across 5 slices
        for {set slice 0} {$slice < 5} {incr slice} {
            set base [expr {$slice * $slice_size}]
            # Add 20 elements per slice
            for {set i 0} {$i < 20} {incr i} {
                r arset myarray [expr {$base + $i * 50}] "s${slice}_e$i"
            }
        }
        assert_equal 100 [r arcount myarray]

        # Delete half from each slice (should cause some demotions)
        for {set slice 0} {$slice < 5} {incr slice} {
            set base [expr {$slice * $slice_size}]
            for {set i 10} {$i < 20} {incr i} {
                r ardel myarray [expr {$base + $i * 50}]
            }
        }
        assert_equal 50 [r arcount myarray]

        # Verify remaining elements
        for {set slice 0} {$slice < 5} {incr slice} {
            set base [expr {$slice * $slice_size}]
            for {set i 0} {$i < 10} {incr i} {
                assert_equal "s${slice}_e$i" [r arget myarray [expr {$base + $i * 50}]]
            }
        }
    }

    test {Stress test - rapid insert/delete cycles} {
        r del myarray

        # Multiple cycles of growth and shrinkage
        for {set cycle 0} {$cycle < 3} {incr cycle} {
            # Grow
            for {set i 0} {$i < 100} {incr i} {
                r arset myarray $i "cycle${cycle}_$i"
            }
            assert_equal 100 [r arcount myarray]

            # Shrink (but leave some)
            for {set i 10} {$i < 100} {incr i} {
                r ardel myarray $i
            }
            assert_equal 10 [r arcount myarray]
        }

        # Verify final state
        for {set i 0} {$i < 10} {incr i} {
            assert_equal "cycle2_$i" [r arget myarray $i]
        }
    }

    # RDB with complex state
    test {RDB persistence with sparse and dense slices} {
        r del myarray

        # Create mix of sparse and dense slices
        # Slice 0: dense (many elements)
        for {set i 0} {$i < 50} {incr i} {
            r arset myarray $i "dense$i"
        }

        # Slice 1 (offset 4096): sparse (few elements)
        r arset myarray 4096 "sparse0"
        r arset myarray 4200 "sparse1"
        r arset myarray 4500 "sparse2"

        # Slice 10 (offset 40960): single element
        r arset myarray 40960 "lonely"

        r bgsave
        waitForBgsave r
        r debug reload

        # Verify all types survived
        for {set i 0} {$i < 50} {incr i} {
            assert_equal "dense$i" [r arget myarray $i]
        }
        assert_equal "sparse0" [r arget myarray 4096]
        assert_equal "sparse1" [r arget myarray 4200]
        assert_equal "sparse2" [r arget myarray 4500]
        assert_equal "lonely" [r arget myarray 40960]
    } {} {needs:debug}

    # Regression test for dense window boundary bug (GitHub issue)
    # When a dense slice window doubles but doesn't reach ar_slice_size,
    # offset + winsize could exceed the slice boundary (4096), causing crashes.
    test {Regression - dense window growth must not exceed slice boundary} {
        r del myarray
        set slice_size 4096

        # Create a dense slice with elements at high offsets within the slice.
        # Start at offset 2100 with a small window, then force growth.
        # Initial window: offset=2100, winsize=64 (or similar small power of 2)
        r arset myarray 2100 "start"

        # Add elements to grow the window to the right.
        # After several doublings, winsize might become 2048.
        # With offset=2100 and winsize=2048, end would be 4148 > 4096 (BUG!)
        # The fix adjusts offset so the window stays within bounds.
        for {set i 2101} {$i < 2200} {incr i} {
            r arset myarray $i "val$i"
        }

        # Now force further right growth that would exceed boundary without fix
        for {set i 2200} {$i < 3500} {incr i 10} {
            r arset myarray $i "val$i"
        }

        # Verify all values are accessible (would crash before the fix)
        assert_equal "start" [r arget myarray 2100]
        assert_equal "val2150" [r arget myarray 2150]
        assert_equal "val3000" [r arget myarray 3000]

        # Verify window respects slice boundary via ARINFO FULL
        set info [r arinfo myarray FULL]
        set avg_size [dict get $info avg-dense-size]
        # With the fix, window should be properly sized (at most slice_size)
        assert {$avg_size <= $slice_size}
    }

    test {Regression - sparse to dense promotion with high offset boundary} {
        r del myarray
        set slice_size 4096

        # Create sparse slice with elements near upper boundary of slice
        # This tests arSparsePromote boundary handling
        for {set i 0} {$i < 8} {incr i} {
            set idx [expr {2200 + $i * 100}]  ;# 2200, 2300, ..., 2900
            r arset myarray $idx "sparse$i"
        }

        # Verify starts as sparse
        set info [r arinfo myarray FULL]
        assert_equal 1 [dict get $info sparse-slices]

        # Add more to trigger promotion - elements span 2200 to 3800
        # Window needs to cover 2200-3800 range (1601 elements span)
        # Without boundary fix, offset=2200 + winsize=2048 = 4248 > 4096 (BUG!)
        for {set i 8} {$i < 20} {incr i} {
            set idx [expr {2200 + $i * 100}]  ;# continues: 3000, 3100, ..., 4100
            r arset myarray $idx "promoted$i"
        }

        # Verify all values survived promotion (would crash before fix)
        for {set i 0} {$i < 8} {incr i} {
            set idx [expr {2200 + $i * 100}]
            assert_equal "sparse$i" [r arget myarray $idx]
        }
        for {set i 8} {$i < 20} {incr i} {
            set idx [expr {2200 + $i * 100}]
            assert_equal "promoted$i" [r arget myarray $idx]
        }
    }

    # Helper to generate random values of different encoding types
    proc random_value {} {
        set type [expr {int(rand() * 4)}]
        switch $type {
            0 {
                # INT encoding: small integers
                set val [expr {int(rand() * 200000) - 100000}]
            }
            1 {
                # FLOAT encoding: synthetic float with random digits
                set int_digits [expr {int(rand() * 5) + 1}]  ;# 1-5 digits before dot
                set frac_digits [expr {int(rand() * 5) + 1}] ;# 1-5 digits after dot
                set int_part ""
                for {set i 0} {$i < $int_digits} {incr i} {
                    append int_part [expr {int(rand() * 10)}]
                }
                set frac_part ""
                for {set i 0} {$i < $frac_digits} {incr i} {
                    append frac_part [expr {int(rand() * 10)}]
                }
                # Add negative sign randomly
                set sign [expr {rand() < 0.5 ? "-" : ""}]
                set val "${sign}${int_part}.${frac_part}"
            }
            2 {
                # SMALLSTR encoding: short strings (1-6 bytes)
                set len [expr {int(rand() * 6) + 1}]
                set val ""
                for {set i 0} {$i < $len} {incr i} {
                    append val [format %c [expr {int(rand() * 26) + 97}]]  ;# a-z
                }
            }
            3 {
                # arString pointer: longer strings (10-30 bytes)
                set len [expr {int(rand() * 21) + 10}]
                set val ""
                for {set i 0} {$i < $len} {incr i} {
                    append val [format %c [expr {int(rand() * 26) + 97}]]  ;# a-z
                }
            }
        }
        return $val
    }

    proc random_array_index {} {
        set roll [expr {int(rand() * 100)}]
        if {$roll < 35} {
            return [expr {int(rand() * 256)}]
        } elseif {$roll < 55} {
            return [expr {4096 + int(rand() * 512)}]
        } elseif {$roll < 75} {
            return [expr {8388608 + int(rand() * 8192)}]
        } elseif {$roll < 90} {
            return [expr {16777216 + int(rand() * 8192)}]
        } else {
            return [expr {int(rand() * 30000000)}]
        }
    }

    proc model_array_delrange {arrname lo hi} {
        upvar 1 $arrname expected

        if {$lo > $hi} {
            set tmp $lo
            set lo $hi
            set hi $tmp
        }

        set deleted 0
        foreach idx [array names expected] {
            if {$idx >= $lo && $idx <= $hi} {
                unset expected($idx)
                incr deleted
            }
        }
        return $deleted
    }

    proc model_array_scan {arrname} {
        upvar 1 $arrname expected

        set result {}
        foreach idx [lsort -integer [array names expected]] {
            lappend result [list $idx $expected($idx)]
        }
        return $result
    }

    proc iterator_stress_rand_between {lo hi} {
        return [expr {$lo + int(rand() * ($hi - $lo + 1))}]
    }

    proc iterator_stress_random_index {slice_size mode} {
        set roll [expr {int(rand() * 100)}]
        switch -- $mode {
            mixed {
                if {$roll < 25} {
                    return [expr {int(rand() * ($slice_size * 2))}]
                } elseif {$roll < 45} {
                    return [expr {$slice_size - 4 + int(rand() * 9)}]
                } elseif {$roll < 60} {
                    return [expr {$slice_size * 2 - 4 + int(rand() * 9)}]
                } elseif {$roll < 78} {
                    return [expr {8388608 + int(rand() * ($slice_size * 2))}]
                } elseif {$roll < 92} {
                    return [expr {16777216 + int(rand() * ($slice_size * 2))}]
                } else {
                    return [expr {int(rand() * 30000000)}]
                }
            }
            dense {
                if {$roll < 60} {
                    return [expr {int(rand() * ($slice_size * 2))}]
                } elseif {$roll < 80} {
                    return [expr {$slice_size - 8 + int(rand() * 17)}]
                } else {
                    return [expr {int(rand() * ($slice_size * 8))}]
                }
            }
            superdir {
                if {$roll < 20} {
                    return [expr {int(rand() * 1024)}]
                } elseif {$roll < 45} {
                    return [expr {8388608 + int(rand() * ($slice_size * 4))}]
                } elseif {$roll < 70} {
                    return [expr {16777216 + int(rand() * ($slice_size * 4))}]
                } elseif {$roll < 90} {
                    return [expr {25165824 + int(rand() * ($slice_size * 4))}]
                } else {
                    return [expr {int(rand() * 40000000)}]
                }
            }
        }
        return [expr {int(rand() * 30000000)}]
    }

    proc iterator_stress_sorted_indices {arrname reverse} {
        upvar 1 $arrname model
        if {$reverse} {
            return [lsort -integer -decreasing [array names model]]
        }
        return [lsort -integer [array names model]]
    }

    proc iterator_stress_scan {arrname start end limit} {
        upvar 1 $arrname model
        set reverse [expr {$start > $end}]
        set lo [expr {$reverse ? $end : $start}]
        set hi [expr {$reverse ? $start : $end}]
        set result {}
        set emitted 0

        foreach idx [iterator_stress_sorted_indices model $reverse] {
            if {$idx < $lo || $idx > $hi} continue
            lappend result [list $idx $model($idx)]
            incr emitted
            if {$limit > 0 && $emitted >= $limit} break
        }
        return $result
    }

    proc iterator_stress_argrep {arrname start end type pattern nocase withvalues limit} {
        upvar 1 $arrname model
        set reverse [expr {$start > $end}]
        set lo [expr {$reverse ? $end : $start}]
        set hi [expr {$reverse ? $start : $end}]
        set pattern_cmp $pattern
        if {$nocase} { set pattern_cmp [string tolower $pattern_cmp] }
        set result {}
        set emitted 0

        foreach idx [iterator_stress_sorted_indices model $reverse] {
            if {$idx < $lo || $idx > $hi} continue
            set value $model($idx)
            set cmp $value
            if {$nocase} { set cmp [string tolower $cmp] }

            if {$type eq "EXACT"} {
                set match [expr {$cmp eq $pattern_cmp}]
            } else {
                set match [expr {[string first $pattern_cmp $cmp] != -1}]
            }

            if {$match} {
                if {$withvalues} {
                    lappend result [list $idx $value]
                } else {
                    lappend result $idx
                }
                incr emitted
                if {$emitted >= $limit} break
            }
        }
        return $result
    }

    proc iterator_stress_arop_used {arrname start end} {
        upvar 1 $arrname model
        set lo [expr {$start > $end ? $end : $start}]
        set hi [expr {$start > $end ? $start : $end}]
        set used 0

        foreach idx [array names model] {
            if {$idx >= $lo && $idx <= $hi} { incr used }
        }
        return $used
    }

    proc iterator_stress_arop_match {arrname start end needle} {
        upvar 1 $arrname model
        set lo [expr {$start > $end ? $end : $start}]
        set hi [expr {$start > $end ? $start : $end}]
        set matches 0

        foreach idx [array names model] {
            if {$idx >= $lo && $idx <= $hi && $model($idx) eq $needle} {
                incr matches
            }
        }
        return $matches
    }

    proc iterator_stress_arop_sum {arrname start end} {
        upvar 1 $arrname model
        set lo [expr {$start > $end ? $end : $start}]
        set hi [expr {$start > $end ? $start : $end}]
        set sum 0.0
        set has_numeric 0

        foreach idx [array names model] {
            if {$idx < $lo || $idx > $hi} continue
            if {[string is double -strict $model($idx)]} {
                set sum [expr {$sum + ($model($idx) + 0.0)}]
                set has_numeric 1
            }
        }

        if {!$has_numeric} { return {} }
        return $sum
    }

    proc iterator_stress_pick_existing_value {arrname} {
        upvar 1 $arrname model
        set keys [array names model]
        if {[llength $keys] == 0} { return [random_value] }
        return $model([lindex $keys [expr {int(rand() * [llength $keys])}]])
    }

    proc iterator_stress_pick_match_pattern {value} {
        set len [string length $value]
        if {$len <= 1} { return $value }
        set start [expr {int(rand() * $len)}]
        set width [expr {1 + int(rand() * ($len - $start))}]
        return [string range $value $start [expr {$start + $width - 1}]]
    }

    proc iterator_stress_flip_case {value} {
        set out ""
        foreach ch [split $value ""] {
            if {![string is alpha -strict $ch] || rand() < 0.5} {
                append out $ch
            } elseif {$ch eq [string tolower $ch]} {
                append out [string toupper $ch]
            } else {
                append out [string tolower $ch]
            }
        }
        return $out
    }

    proc iterator_stress_check_equal {label expected got} {
        if {$expected ne $got} {
            fail "$label mismatch - expected '$expected', got '$got'"
        }
    }

    proc iterator_stress_check_sum {label expected got} {
        if {$expected eq {} || $got eq {}} {
            if {$expected ne $got} {
                fail "$label mismatch - expected '$expected', got '$got'"
            }
            return
        }

        if {abs(($expected + 0.0) - ($got + 0.0)) > 1e-9} {
            fail "$label mismatch - expected '$expected', got '$got'"
        }
    }

    proc iterator_stress_validate {r arrname slice_size mode tag step full_scan} {
        upvar 1 $arrname model
        set count [array size model]

        if {$count == 0} {
            iterator_stress_check_equal "$tag/$step exists" 0 [r exists myarray]
            if {$full_scan} {
                iterator_stress_check_equal "$tag/$step empty-scan" {} \
                    [r arscan myarray 0 50000000]
            }
            return
        }

        iterator_stress_check_equal "$tag/$step count" $count [r arcount myarray]
        if {$full_scan} {
            set start [expr {$step % 2 == 0 ? 0 : 50000000}]
            set end [expr {$step % 2 == 0 ? 50000000 : 0}]
            iterator_stress_check_equal "$tag/$step full-scan" \
                [iterator_stress_scan model $start $end 0] \
                [r arscan myarray $start $end]
        }

        for {set probe 0} {$probe < 2} {incr probe} {
            set start [iterator_stress_random_index $slice_size $mode]
            set end [iterator_stress_random_index $slice_size $mode]
            if {rand() < 0.15} { set start 0 }
            if {rand() < 0.15} { set end 50000000 }

            set limit [iterator_stress_rand_between 1 10]
            iterator_stress_check_equal "$tag/$step scan/$probe" \
                [iterator_stress_scan model $start $end $limit] \
                [r arscan myarray $start $end LIMIT $limit]

            set grep_type [expr {rand() < 0.5 ? "EXACT" : "MATCH"}]
            if {rand() < 0.7} {
                set pattern [iterator_stress_pick_existing_value model]
                if {$grep_type eq "MATCH"} {
                    set pattern [iterator_stress_pick_match_pattern $pattern]
                }
            } else {
                set pattern [random_value]
            }

            set withvalues [expr {rand() < 0.5}]
            set nocase [expr {rand() < 0.5}]
            if {$nocase} { set pattern [iterator_stress_flip_case $pattern] }
            set grep_limit [iterator_stress_rand_between 1 8]
            set grep_cmd [list r argrep myarray $start $end $grep_type $pattern LIMIT $grep_limit]
            if {$withvalues} { lappend grep_cmd WITHVALUES }
            if {$nocase} { lappend grep_cmd NOCASE }

            iterator_stress_check_equal "$tag/$step argrep/$probe" \
                [iterator_stress_argrep model $start $end $grep_type $pattern $nocase $withvalues $grep_limit] \
                [uplevel 1 $grep_cmd]

            iterator_stress_check_equal "$tag/$step used/$probe" \
                [iterator_stress_arop_used model $start $end] \
                [r arop myarray $start $end USED]

            set needle [iterator_stress_pick_existing_value model]
            iterator_stress_check_equal "$tag/$step match/$probe" \
                [iterator_stress_arop_match model $start $end $needle] \
                [r arop myarray $start $end MATCH $needle]

            iterator_stress_check_sum "$tag/$step sum/$probe" \
                [iterator_stress_arop_sum model $start $end] \
                [r arop myarray $start $end SUM]
        }
    }

    proc iterator_stress_apply_operation {r arrname slice_size mode} {
        upvar 1 $arrname model
        set roll [expr {int(rand() * 100)}]

        if {$roll < 30} {
            set idx [iterator_stress_random_index $slice_size $mode]
            set val [random_value]
            r arset myarray $idx $val
            set model($idx) $val
        } elseif {$roll < 45} {
            set start [iterator_stress_random_index $slice_size $mode]
            set values {}
            set len [iterator_stress_rand_between 2 8]

            for {set i 0} {$i < $len} {incr i} {
                set val [random_value]
                lappend values $val
                set model([expr {$start + $i}]) $val
            }
            r arset myarray $start {*}$values
        } elseif {$roll < 58} {
            set idx [iterator_stress_random_index $slice_size $mode]
            r ardel myarray $idx
            catch {unset model($idx)}
        } elseif {$roll < 78} {
            set args {}
            set nranges [iterator_stress_rand_between 1 3]

            for {set i 0} {$i < $nranges} {incr i} {
                set lo [iterator_stress_random_index $slice_size $mode]
                set hi [iterator_stress_random_index $slice_size $mode]
                lappend args $lo $hi
                model_array_delrange model $lo $hi
            }
            r ardelrange myarray {*}$args
        } elseif {$roll < 90} {
            set base [expr {[iterator_stress_random_index $slice_size $mode] / $slice_size * $slice_size}]
            set start [expr {$base + [iterator_stress_rand_between 0 [expr {$slice_size > 16 ? 16 : $slice_size - 1}]]}]
            set values {}
            set len [iterator_stress_rand_between 4 10]

            for {set i 0} {$i < $len} {incr i} {
                set val [random_value]
                lappend values $val
                set model([expr {$start + $i}]) $val
            }
            r arset myarray $start {*}$values
        } else {
            set base [expr {[iterator_stress_random_index $slice_size $mode] / $slice_size * $slice_size}]
            set lo [expr {$base + [iterator_stress_rand_between 0 [expr {$slice_size > 24 ? 24 : $slice_size - 1}]]}]
            set hi [expr {$base + [iterator_stress_rand_between 0 [expr {$slice_size > 24 ? 24 : $slice_size - 1}]]}]
            model_array_delrange model $lo $hi
            r ardelrange myarray $lo $hi
        }
    }

    # Random testing - most effective way to find edge case bugs
    test {Random testing - staged write/delete workload with verification} {
        r flushdb
        expr {srand(12345)}  ;# Fixed seed for reproducibility
        set max_idx 5000  ;# Range of possible indices
        set ops_per_stage 200  ;# Operations per stage

        # Tcl-side tracking of expected state
        array set expected {}

        # 11 stages with decreasing write ratio
        # Stage 0: 100% writes, Stage 10: 0% writes 100% deletes
        set stages {
            {100 0}
            {90 10}
            {80 20}
            {70 30}
            {60 40}
            {50 50}
            {40 60}
            {30 70}
            {20 80}
            {10 90}
            {0 100}
        }

        set stage_num 0
        foreach stage $stages {
            set write_pct [lindex $stage 0]

            for {set op 0} {$op < $ops_per_stage} {incr op} {
                set roll [expr {int(rand() * 100)}]
                set idx [expr {int(rand() * $max_idx)}]

                if {$roll < $write_pct} {
                    # Write operation with random value type
                    set val [random_value]
                    r arset myarray $idx $val
                    set expected($idx) $val
                } else {
                    # Delete operation - always send to Redis, track locally
                    r ardel myarray $idx
                    if {[info exists expected($idx)]} {
                        unset expected($idx)
                    }
                }
            }

            # Verify entire array matches expected state
            set expected_count [array size expected]
            if {[r exists myarray]} {
                set actual_count [r arcount myarray]
            } else {
                set actual_count 0
            }

            if {$expected_count != $actual_count} {
                fail "Stage $stage_num: count mismatch - expected $expected_count, got $actual_count"
            }

            # Verify all expected values individually
            foreach idx [array names expected] {
                set got [r arget myarray $idx]
                if {$got ne $expected($idx)} {
                    fail "Stage $stage_num: idx $idx - expected '$expected($idx)', got '$got'"
                }
            }

            incr stage_num
        }

        # Final cleanup: delete all remaining expected entries
        foreach idx [array names expected] {
            r ardel myarray $idx
            unset expected($idx)
        }

        # After cleanup, array should be empty/deleted
        assert_equal 0 [r exists myarray]
    }

    test {Random testing - large scale with RDB verification} {
        r flushdb
        expr {srand(54321)}  ;# Fixed seed for reproducibility
        set max_idx 100000  ;# Range to test multiple slices
        set num_writes 2000

        # Tcl-side tracking
        array set expected {}

        # Phase 1: Random writes with mixed value types
        for {set i 0} {$i < $num_writes} {incr i} {
            set idx [expr {int(rand() * $max_idx)}]
            set val [random_value]
            r arset myarray $idx $val
            set expected($idx) $val
        }

        set expected_count [array size expected]
        set count_before [r arcount myarray]
        assert_equal $expected_count $count_before

        # Save and reload
        r bgsave
        waitForBgsave r
        r debug reload

        # Verify count preserved
        assert_equal $count_before [r arcount myarray]

        # Verify all expected values
        foreach idx [array names expected] {
            set got [r arget myarray $idx]
            if {$got ne $expected($idx)} {
                fail "After reload: idx $idx - expected '$expected($idx)', got '$got'"
            }
        }

        # Phase 2: Random deletes (delete half)
        set keys_list [array names expected]
        set delete_count [expr {[llength $keys_list] / 2}]
        for {set i 0} {$i < $delete_count} {incr i} {
            set idx [lindex $keys_list $i]
            r ardel myarray $idx
            unset expected($idx)
        }

        # Verify remaining
        set remaining [array size expected]
        assert_equal $remaining [r arcount myarray]

        foreach idx [array names expected] {
            assert_equal $expected($idx) [r arget myarray $idx]
        }
    } {} {needs:debug}

    test {Random testing - iterator model stress across dense sparse and superdir} {
        set orig_slice_size [lindex [r config get array-slice-size] 1]
        set orig_kmax [lindex [r config get array-sparse-kmax] 1]
        set orig_kmin [lindex [r config get array-sparse-kmin] 1]
        set scenarios {
            {mixed-default 4096 10 5 mixed 120 111}
            {small-slices 256 6 3 dense 140 333}
            {superdir-heavy 1024 8 4 superdir 160 555}
            {superdir-heavy 1024 8 4 superdir 160 666}
        }

        set err [catch {
            foreach scenario $scenarios {
                lassign $scenario name slice_size kmax kmin mode steps seed
                r flushdb
                r config set array-sparse-kmax $kmax
                r config set array-sparse-kmin $kmin
                r config set array-slice-size $slice_size
                expr {srand($seed)}
                catch {array unset model}
                array set model {}

                # Start each scenario with the exact superdir shape that
                # previously exposed iterator progress bugs.
                r arset myarray 43 a
                set model(43) a
                r arset myarray [expr {$slice_size + 490}] b
                set model([expr {$slice_size + 490}]) b
                r arset myarray 19245258 c
                set model(19245258) c

                iterator_stress_validate r model $slice_size $mode "$name/$seed" -1 1

                for {set step 0} {$step < $steps} {incr step} {
                    iterator_stress_apply_operation r model $slice_size $mode
                    iterator_stress_validate r model $slice_size $mode \
                        "$name/$seed" $step [expr {$step % 20 == 0}]
                }
            }
        } msg opts]

        r flushdb
        r config set array-sparse-kmax $orig_kmax
        r config set array-sparse-kmin $orig_kmin
        r config set array-slice-size $orig_slice_size

        if {$err} {
            return -options $opts $msg
        }
    }

    # =========================================================================
    # Circular buffer (ring buffer) comprehensive tests
    # =========================================================================

    test {Circular buffer - ARRING basic wraparound} {
        r del myarray
        # Insert 20 values with MOD 10 - should wrap around twice
        for {set i 0} {$i < 20} {incr i} {
            set result [r arring myarray 10 "val$i"]
            assert_equal [expr {$i % 10}] $result
        }
        # Should have exactly 10 elements (0-9)
        assert_equal 10 [r arcount myarray]
        # Values should be the last 10 inserted (val10-val19)
        for {set i 0} {$i < 10} {incr i} {
            assert_equal "val[expr {$i + 10}]" [r arget myarray $i]
        }
    }

    test {Circular buffer - ARRING with size 1} {
        r del myarray
        # MOD 1 means only ever keep one element at index 0
        for {set i 0} {$i < 100} {incr i} {
            r arring myarray 1 "val$i"
        }
        assert_equal 1 [r arcount myarray]
        assert_equal "val99" [r arget myarray 0]
    }

    test {Circular buffer - ARRING preserves insert_idx through RDB} {
        r del myarray
        # Create a circular buffer, wrap around a few times
        for {set i 0} {$i < 15} {incr i} {
            r arring myarray 5 "val$i"
        }
        # insert_idx should now be 0 (15 % 5 = 0)
        set next_before [r arnext myarray]

        # Save and reload
        r bgsave
        waitForBgsave r
        r debug reload

        # Verify insert_idx is preserved
        assert_equal $next_before [r arnext myarray]

        # Continue inserting - should continue from where it left off
        r arring myarray 5 "after_reload"
        # The next insert should be at position 1 (since we were at 0)
        assert_equal "after_reload" [r arget myarray [expr {$next_before % 5}]]
    } {} {needs:debug}

    test {Circular buffer - ARLASTITEMS with wraparound} {
        r del myarray
        # Create circular buffer with 8 items, MOD 5
        for {set i 0} {$i < 8} {incr i} {
            r arring myarray 5 $i
        }
        # Values: 0->3, 1->4, 2->5, 3->6, 4->7
        # insert_idx = 3 (8 % 5 = 3)

        # ARLASTITEMS should return the N most recently inserted
        set result [r arlastitems myarray 3]
        # Last 3 inserted: 7, 6, 5 - in chronological order: 5, 6, 7
        assert_equal {5 6 7} $result

        # With REV flag
        set result [r arlastitems myarray 3 REV]
        assert_equal {7 6 5} $result

        # Request more items than exist
        set result [r arlastitems myarray 10]
        assert_equal 5 [llength $result]
    }

    test {Circular buffer - ARLASTITEMS handles empty and partial cases} {
        r del myarray
        # Empty array
        set result [r arlastitems myarray 5]
        assert_equal {} $result

        # Fewer items than requested (no wraparound yet)
        r arring myarray 10 a
        r arring myarray 10 b
        r arring myarray 10 c

        set result [r arlastitems myarray 5]
        assert_equal {a b c} $result
    }

    test {Circular buffer - ARNEXT tracks correctly with ARRING} {
        r del myarray
        # Insert with MOD, tracking position
        # MOD wraps the insert position but ARNEXT continues until next wrap
        for {set i 0} {$i < 7} {incr i} {
            set expected_idx [expr {$i % 4}]
            set result [r arring myarray 4 $i]
            assert_equal $expected_idx $result
            # ARNEXT: after a wraparound insert, it's expected_idx+1
            # Otherwise it's the running counter+1 until it wraps
            if {$i < 4} {
                # Before first wrap, ARNEXT is i+1
                assert_equal [expr {$i + 1}] [r arnext myarray]
            } else {
                # After wrap, ARNEXT is (position+1)
                assert_equal [expr {$expected_idx + 1}] [r arnext myarray]
            }
        }
    }

    test {Circular buffer - ARSEEK followed by ARRING} {
        r del myarray
        # Start inserting
        r arinsert myarray a
        r arinsert myarray b
        r arinsert myarray c
        # insert_idx = 2, next = 3

        # Seek to position 10
        r arseek myarray 10
        assert_equal 10 [r arnext myarray]

        # Now use MOD - should reset behavior
        r arring myarray 5 x
        # This should insert at index 0 (10 % 5 = 0)
        assert_equal x [r arget myarray 0]
    }

    test {Circular buffer - ARSEEK 0 is honored on ARRING grow} {
        r del myarray
        for {set i 0} {$i < 5} {incr i} {
            r arring myarray 3 "ring$i"
        }

        assert_equal 1 [r arseek myarray 0]
        r arring myarray 8 "grown"

        # ARSEEK 0 is an explicit cursor override, so grow should not repack
        # first: the next ARRING write still goes to index 0.
        assert_equal "grown" [r arget myarray 0]
        assert_equal "ring4" [r arget myarray 1]
        assert_equal "ring2" [r arget myarray 2]
        assert_equal 1 [r arnext myarray]
    }

    test {Circular buffer - ARRING growth uses new capacity after wrap} {
        r del myarray
        for {set i 0} {$i < 8} {incr i} {
            r arring myarray 5 "v$i"
        }
        # Current ring window contains the latest 5 values:
        # v3 v4 v5 v6 v7, with insert_idx at position 2.

        r arring myarray 8 "grown"

        # Growing must compact the wrapped ring first, so the new value uses
        # the newly added capacity instead of overwriting low indexes again.
        assert_equal "v3" [r arget myarray 0]
        assert_equal "v4" [r arget myarray 1]
        assert_equal "v5" [r arget myarray 2]
        assert_equal "v6" [r arget myarray 3]
        assert_equal "v7" [r arget myarray 4]
        assert_equal "grown" [r arget myarray 5]
        assert_equal 6 [r arnext myarray]
    }

    test {Circular buffer - Mixed ARSET and ARRING immediately restores ring size} {
        r del myarray
        # Use MOD to create ring buffer
        for {set i 0} {$i < 5} {incr i} {
            r arring myarray 3 "ring$i"
        }
        # After 5 inserts with MOD 3:
        # Position 0: ring0 -> ring3 (overwritten)
        # Position 1: ring1 -> ring4 (overwritten)
        # Position 2: ring2
        # insert_idx=1, next=2

        # Now manually set a value outside the ring
        r arset myarray 100 "outside"

        # Ring buffer values should still be there
        assert_equal "ring3" [r arget myarray 0]
        assert_equal "ring4" [r arget myarray 1]
        assert_equal "ring2" [r arget myarray 2]
        assert_equal "outside" [r arget myarray 100]

        # Continue ring buffer. The ring size should be re-established
        # immediately, so values outside the 0..2 window disappear at once.
        r arring myarray 3 "ring5"
        assert_equal 3 [r arcount myarray]
        assert_equal {} [r arget myarray 100]
        assert_equal "ring5" [r arget myarray 0]
    }

    test {Circular buffer - insert_idx survives RDB with complex state} {
        r del myarray
        # Create circular buffer across multiple slices
        for {set i 0} {$i < 100} {incr i} {
            # Use large MOD to spread across slices
            r arring myarray 50 "v$i"
        }

        set info_before [r arinfo myarray]
        set next_before [r arnext myarray]
        set count_before [r arcount myarray]

        # Also set some values outside the ring
        r arset myarray 10000 "far_away"

        # Save and reload
        r bgsave
        waitForBgsave r
        r debug reload

        # Verify state preserved
        assert_equal $count_before [expr {[r arcount myarray] - 1}]  ;# -1 for far_away
        assert_equal $next_before [r arnext myarray]
        assert_equal "far_away" [r arget myarray 10000]

        # Verify ring buffer content - last 50 values should be v50-v99
        for {set i 0} {$i < 50} {incr i} {
            assert_equal "v[expr {$i + 50}]" [r arget myarray $i]
        }
    } {} {needs:debug}

    test {Circular buffer - ARLASTITEMS reverse order} {
        r del myarray
        # Create ring with wraparound
        for {set i 0} {$i < 12} {incr i} {
            r arring myarray 8 "v$i"
        }
        # After 12 inserts MOD 8:
        # insert_idx = 12 % 8 = 4 - 1 = 3 (last inserted at position 3)
        # Values: positions 0-7 contain v4-v11

        # ARLASTITEMS returns most recent items in chronological order
        set result [r arlastitems myarray 4]
        # Last 4 inserted were v11, v10, v9, v8 - returned oldest to newest
        assert_equal {v8 v9 v10 v11} $result

        # With REV flag - returned newest to oldest
        set result [r arlastitems myarray 4 REV]
        assert_equal {v11 v10 v9 v8} $result

        # Request all items
        set result [r arlastitems myarray 100]
        assert_equal 8 [llength $result]
    }

    test {Circular buffer - ARRING truncation when size decreases} {
        r del myarray
        # Create ring buffer with MOD 10
        for {set i 0} {$i < 15} {incr i} {
            r arring myarray 10 "v$i"
        }
        # Now have 10 elements at positions 0-9
        # After 15 inserts: 0->v10, 1->v11, ..., 4->v14, 5->v5, ..., 9->v9
        assert_equal 10 [r arcount myarray]

        # Use smaller MOD - this truncates to positions 0-4 AND inserts new value
        # The new insert goes to position (15 % 5) = 0, replacing v10
        r arring myarray 5 "truncated"
        # Now have only 5 elements (positions 0-4), with position 0 = "truncated"
        assert_equal 5 [r arcount myarray]

        # Verify values
        assert_equal "truncated" [r arget myarray 0]  ;# new value
        assert_equal "v11" [r arget myarray 1]
        assert_equal "v12" [r arget myarray 2]
        assert_equal "v13" [r arget myarray 3]
        assert_equal "v14" [r arget myarray 4]

        # Positions 5-9 should be empty (truncated)
        assert_equal {} [r arget myarray 5]
        assert_equal {} [r arget myarray 9]
    }

    test {Circular buffer - ARRING shrink stops at first hole} {
        r del myarray
        for {set i 0} {$i < 5} {incr i} {
            r arring myarray 5 "v$i"
        }

        r ardel myarray 3
        r arring myarray 3 "new"

        assert_equal 2 [r arcount myarray]
        assert_equal "v4" [r arget myarray 0]
        assert_equal "new" [r arget myarray 1]
        assert_equal {} [r arget myarray 2]
    }

    test {Circular buffer - ARRING grow stops at first hole} {
        r del myarray
        for {set i 0} {$i < 8} {incr i} {
            r arring myarray 5 "v$i"
        }

        r ardel myarray 1
        r arring myarray 8 "grown"

        assert_equal 2 [r arcount myarray]
        assert_equal "v7" [r arget myarray 0]
        assert_equal "grown" [r arget myarray 1]
        assert_equal {} [r arget myarray 2]
    }

    test {Circular buffer - ARLASTITEMS with various counts and REV} {
        r del myarray
        # Create simple ring buffer
        for {set i 0} {$i < 20} {incr i} {
            r arring myarray 10 "item$i"
        }
        # Contains item10-item19 at positions 0-9

        # Get exactly 1 item
        assert_equal {item19} [r arlastitems myarray 1]
        assert_equal {item19} [r arlastitems myarray 1 REV]

        # Get 3 items
        set result [r arlastitems myarray 3]
        assert_equal {item17 item18 item19} $result
        set result [r arlastitems myarray 3 REV]
        assert_equal {item19 item18 item17} $result

        # Get all 10 items
        set result [r arlastitems myarray 10]
        assert_equal 10 [llength $result]
        assert_equal "item10" [lindex $result 0]
        assert_equal "item19" [lindex $result end]

        # REV order for all items
        set result [r arlastitems myarray 10 REV]
        assert_equal "item19" [lindex $result 0]
        assert_equal "item10" [lindex $result end]
    }

    test {Circular buffer - ARLASTITEMS edge cases} {
        r del myarray
        # Empty array
        assert_equal {} [r arlastitems myarray 5]
        assert_equal {} [r arlastitems myarray 5 REV]

        # Single element
        r arinsert myarray "only"
        assert_equal {only} [r arlastitems myarray 1]
        assert_equal {only} [r arlastitems myarray 10]
        assert_equal {only} [r arlastitems myarray 1 REV]

        # Two elements - no wraparound yet
        r arinsert myarray "second"
        assert_equal {only second} [r arlastitems myarray 5]
        assert_equal {second only} [r arlastitems myarray 5 REV]
    }

    # ============================================================
    # Regression tests for bugs found during code review
    # ============================================================

    test {Regression #3 - arTruncate must decrement count correctly} {
        r del myarray
        # Fill array with 20 elements
        for {set i 0} {$i < 20} {incr i} {
            r arset myarray $i "val$i"
        }
        assert_equal 20 [r arcount myarray]

        # Use ARRING to trigger truncation
        # First set insert_idx to 15, then insert with MOD 10
        r arseek myarray 16
        r arring myarray 10 "wrap"

        # After MOD 10 truncation, only indices 0-9 should exist
        # The count should be <= 10 (some original values + new one)
        set count [r arcount myarray]
        assert_lessthan $count 11  ;# count <= 10

        # Verify elements >= 10 are gone
        assert_equal {} [r arget myarray 10]
        assert_equal {} [r arget myarray 15]
        assert_equal {} [r arget myarray 19]
    }

    test {Regression #5 - AROP MATCH with large strings (>256 bytes)} {
        r del myarray
        # Create a string larger than 256 bytes
        set largestr [string repeat "x" 300]
        set largestr2 [string repeat "y" 300]

        r arset myarray 0 $largestr
        r arset myarray 1 "small"
        r arset myarray 2 $largestr
        r arset myarray 3 $largestr2

        # MATCH should find exactly 2 occurrences of largestr
        assert_equal 2 [r arop myarray 0 3 MATCH $largestr]
        assert_equal 1 [r arop myarray 0 3 MATCH $largestr2]
        assert_equal 1 [r arop myarray 0 3 MATCH "small"]
        assert_equal 0 [r arop myarray 0 3 MATCH "notfound"]
    }

    test {Regression #6 - DEBUG DIGEST with large strings (>256 bytes)} {
        r del myarray
        set largestr [string repeat "z" 500]
        r arset myarray 0 $largestr
        r arset myarray 1 "small"
        r arset myarray 100 [string repeat "w" 1000]

        # Get digest - should not crash and should be deterministic
        set d1 [r debug digest-value myarray]
        set d2 [r debug digest-value myarray]
        assert_equal $d1 $d2 "Digest should be deterministic"

        # Modify and verify digest changes
        r arset myarray 0 "changed"
        set d3 [r debug digest-value myarray]
        if {$d1 eq $d3} {
            fail "Digest should change after modification"
        }
    } {} {needs:debug}

    test {Regression #7 - RDB with negative integers including -1} {
        r flushdb
        # -1 was problematic because it became UINT64_MAX which was RDB_LENERR
        r arset myarray 0 -1
        r arset myarray 1 -100
        r arset myarray 2 -9223372036854775808  ;# INT64_MIN as string
        r arset myarray 3 0
        r arset myarray 4 1
        r arset myarray 5 9223372036854775807   ;# INT64_MAX as string

        set d1 [r debug digest-value myarray]

        # Save and reload
        r bgsave
        waitForBgsave r
        r debug reload

        # Verify values survived
        assert_equal -1 [r arget myarray 0]
        assert_equal -100 [r arget myarray 1]
        # Note: very large integers may be stored as strings
        assert_equal 0 [r arget myarray 3]
        assert_equal 1 [r arget myarray 4]

        set d2 [r debug digest-value myarray]
        assert_equal $d1 $d2 "Digest should match after RDB reload"
    } {} {needs:debug}

    test {Regression #10 - ARSEEK on non-existent key should not create it} {
        r del myarray
        # ARSEEK on non-existent key
        assert_equal 0 [r arseek myarray 100]

        # Key should NOT exist
        assert_equal 0 [r exists myarray]

        # Now create the array and verify ARSEEK works
        r arinsert myarray "first"
        assert_equal 1 [r exists myarray]

        # ARSEEK on existing key should work
        assert_equal 1 [r arseek myarray 50]
        r arinsert myarray "second"
        assert_equal 51 [r arnext myarray]
    }

    test {Regression #12 - ARMGET/ARGETRANGE return WRONGTYPE on wrong type} {
        r del myarray
        r set myarray "string_value"

        # ARMGET should return WRONGTYPE error
        assert_error {WRONGTYPE*} {r armget myarray 0 1 2}

        # ARGETRANGE should return WRONGTYPE error
        assert_error {WRONGTYPE*} {r argetrange myarray 0 10}

        # Cleanup
        r del myarray
    }

    test {Regression - RDB preserves exact numeric string forms} {
        r flushdb
        set values [list \
            0 "3.141592653589793" \
            1 "-2.718281828459045" \
            2 "1.0e-10" \
            3 "1.0e+100"]

        foreach {idx val} $values {
            r arset myarray $idx $val
        }

        foreach {idx val} $values {
            assert_equal $val [r arget myarray $idx]
        }

        # Save and reload
        r bgsave
        waitForBgsave r
        r debug reload

        foreach {idx val} $values {
            assert_equal $val [r arget myarray $idx]
        }
    } {} {needs:debug}

    test {Whole-number floats with .0 suffix encode as inline floats} {
        # Values like "1.0" should be encoded as inline floats, not heap strings.
        # This tests the ".0" suffix optimization in arTryEncodeFloat.
        r del myarray

        # Various whole-number floats that should round-trip with ".0"
        r arset myarray 0 1.0
        r arset myarray 1 -1.0
        r arset myarray 2 0.0
        r arset myarray 3 42.0
        r arset myarray 4 -42.0
        r arset myarray 5 1000000.0
        r arset myarray 6 -9999999.0

        # Verify exact round-trip (the ".0" must be preserved)
        assert_equal "1.0" [r arget myarray 0]
        assert_equal "-1.0" [r arget myarray 1]
        assert_equal "0.0" [r arget myarray 2]
        assert_equal "42.0" [r arget myarray 3]
        assert_equal "-42.0" [r arget myarray 4]
        assert_equal "1000000.0" [r arget myarray 5]
        assert_equal "-9999999.0" [r arget myarray 6]

        # Verify these survive RDB save/reload (confirms they're properly encoded)
        r bgsave
        waitForBgsave r
        r debug reload

        assert_equal "1.0" [r arget myarray 0]
        assert_equal "-1.0" [r arget myarray 1]
        assert_equal "0.0" [r arget myarray 2]
        assert_equal "42.0" [r arget myarray 3]
        assert_equal "-42.0" [r arget myarray 4]
        assert_equal "1000000.0" [r arget myarray 5]
        assert_equal "-9999999.0" [r arget myarray 6]
    } {} {needs:debug}

    test {Integer values without .0 still encode as integers, not floats} {
        # Ensure "1" (without decimal) is encoded as integer, not float
        r del myarray

        r arset myarray 0 1
        r arset myarray 1 -1
        r arset myarray 2 0
        r arset myarray 3 42
        r arset myarray 4 9999999

        # Values without ".0" should stay as integers
        assert_equal "1" [r arget myarray 0]
        assert_equal "-1" [r arget myarray 1]
        assert_equal "0" [r arget myarray 2]
        assert_equal "42" [r arget myarray 3]
        assert_equal "9999999" [r arget myarray 4]

        # Verify RDB round-trip preserves them as integers
        r bgsave
        waitForBgsave r
        r debug reload

        assert_equal "1" [r arget myarray 0]
        assert_equal "-1" [r arget myarray 1]
        assert_equal "0" [r arget myarray 2]
        assert_equal "42" [r arget myarray 3]
        assert_equal "9999999" [r arget myarray 4]
    } {} {needs:debug}

    test {AROP on whole-number floats works correctly} {
        # Verify AROP aggregation works on values encoded with the .0 optimization
        r del myarray

        r arset myarray 0 10.0
        r arset myarray 1 20.0
        r arset myarray 2 30.0

        # SUM should work on whole-number floats (AROP returns computed values)
        assert_equal 60 [r arop myarray 0 2 SUM]

        # MIN/MAX should work
        assert_equal 10 [r arop myarray 0 2 MIN]
        assert_equal 30 [r arop myarray 0 2 MAX]

        # MATCH should find the encoded values
        assert_equal 1 [r arop myarray 0 2 MATCH 10.0]
        assert_equal 1 [r arop myarray 0 2 MATCH 20.0]
    }

    test {Exact string recovery survives AOF rewrite} {
        r flushdb
        set longstr [string repeat x 100]
        set values [list \
            0 "1.0" \
            1 "-1.0" \
            2 "42.0" \
            3 "hello" \
            4 "12345" \
            5 "-0.0" \
            6 "0.00" \
            7 "10.500" \
            8 "001.25" \
            9 "1.0e-10" \
            10 "1.0e+100" \
            11 $longstr \
            12 ""]

        foreach {idx val} $values {
            r arset myarray $idx $val
        }

        foreach {idx val} $values {
            assert_equal $val [r arget myarray $idx]
        }

        # Trigger AOF rewrite and reload
        r bgrewriteaof
        waitForBgrewriteaof r
        r debug loadaof

        foreach {idx val} $values {
            assert_equal $val [r arget myarray $idx]
        }
    } {} {needs:debug}

    test {Regression - CONFIG GET/SET for array settings} {
        # Verify config options exist and are readable
        set slice_size [lindex [r config get array-slice-size] 1]
        set sparse_kmax [lindex [r config get array-sparse-kmax] 1]
        set sparse_kmin [lindex [r config get array-sparse-kmin] 1]

        # Verify defaults
        assert_equal 4096 $slice_size
        assert_equal 10 $sparse_kmax
        assert_equal 5 $sparse_kmin

        # sparse-kmax and sparse-kmin should be modifiable
        r config set array-sparse-kmax 20
        assert_equal 20 [lindex [r config get array-sparse-kmax] 1]
        r config set array-sparse-kmax $sparse_kmax  ;# restore

        r config set array-sparse-kmin 8
        assert_equal 8 [lindex [r config get array-sparse-kmin] 1]
        r config set array-sparse-kmin $sparse_kmin  ;# restore

        # slice-size is modifiable but must be a power of two
        r config set array-slice-size 8192
        assert_equal 8192 [lindex [r config get array-slice-size] 1]
        r config set array-slice-size $slice_size  ;# restore

        # Non-power-of-two should error
        assert_error {*power of two*} {r config set array-slice-size 5000}
    }

    test {Arrays created with different slice sizes work after config change} {
        # Create an array with current slice size
        r del myarray
        set orig_size [lindex [r config get array-slice-size] 1]

        # Create array and populate it
        for {set i 0} {$i < 10000} {incr i 1000} {
            r arset myarray $i "value_$i"
        }
        set orig_count [r arcount myarray]

        # Change slice size - existing arrays should keep working
        r config set array-slice-size 8192

        # Verify old array still works
        assert_equal $orig_count [r arcount myarray]
        assert_equal "value_0" [r arget myarray 0]
        assert_equal "value_5000" [r arget myarray 5000]
        assert_equal "value_9000" [r arget myarray 9000]

        # Create new array with new slice size
        r del newarray
        r arset newarray 0 "new_value"
        assert_equal "new_value" [r arget newarray 0]

        # Restore config
        r config set array-slice-size $orig_size
        r del myarray
        r del newarray
    }

    test {Regression - AOF rewrite with superdir mode (high indices)} {
        # This tests the fix for AOF rewrite not iterating superdir blocks.
        # With slice_size=4096, slice_id 2048 starts at index 8388608.
        # Indices >= 8388608 trigger superdir mode.

        r del aoftest

        # Create array with elements that trigger superdir mode
        r arset aoftest 0 base
        r arset aoftest 8388608 triggers_superdir
        r arset aoftest 50000000 high
        r arset aoftest 100000000 very_high

        assert_equal 4 [r arcount aoftest]

        # Verify superdir mode is active (directory-size shows number of blocks)
        set info [r arinfo aoftest]
        set dir_size [dict get $info directory-size]
        # With these indices across multiple superdir blocks, dir_size should be > 1
        assert {$dir_size >= 1}

        # Trigger AOF rewrite and reload (same pattern as other AOF tests)
        r bgrewriteaof
        waitForBgrewriteaof r
        r debug loadaof

        # Verify data survived AOF rewrite and reload
        assert_equal 4 [r arcount aoftest]
        assert_equal "base" [r arget aoftest 0]
        assert_equal "triggers_superdir" [r arget aoftest 8388608]
        assert_equal "high" [r arget aoftest 50000000]
        assert_equal "very_high" [r arget aoftest 100000000]

        assert_equal 1 [r del aoftest]
    } {} {needs:debug}

    # =========================================================================
    # Superdir command coverage
    # =========================================================================

    test {ARGETRANGE works across a superdir slice boundary} {
        r del myarray

        # Cross slice 2047 -> 2048. Inserting the high index forces the array
        # into superdir mode, but the range itself is still short.
        r arset myarray 8388607 "left"
        r arset myarray 8388608 "mid"
        r arset myarray 8388609 "right"

        assert_equal {left mid right} [r argetrange myarray 8388607 8388609]
        assert_equal {right mid left} [r argetrange myarray 8388609 8388607]
    }

    test {ARSET pre-promotes sparse slice in superdir mode} {
        r del myarray
        set kmax [lindex [r config get array-sparse-kmax] 1]
        assert {$kmax >= 4}

        # Build a sparse slice with kmax-1 existing elements at even offsets.
        # The later range write covers offsets 0..kmax-1, so some of these
        # positions are already filled and some are new.
        for {set i 0} {$i < $kmax - 1} {incr i} {
            set off [expr {$i * 2}]
            r arset myarray [expr {8388608 + $off}] "old$off"
        }

        set info [r arinfo myarray FULL]
        assert_equal 0 [dict get $info dense-slices]
        assert_equal 1 [dict get $info sparse-slices]

        # The range has kmax slots, while the slice already contains kmax-1
        # elements spread across the slice. This keeps range_size <= kmax, so
        # the helper must take the count+new_elements path in order to decide
        # the promotion.
        set values {}
        set existing_in_range 0
        for {set off 0} {$off < $kmax} {incr off} {
            lappend values "n$off"
            if {$off % 2 == 0 && $off <= 2 * ($kmax - 2)} {
                incr existing_in_range
            }
        }
        set expected_new [expr {$kmax - $existing_in_range}]
        assert_equal $expected_new [r arset myarray 8388608 {*}$values]

        set info [r arinfo myarray FULL]
        assert_equal 1 [dict get $info dense-slices]
        assert_equal 0 [dict get $info sparse-slices]
        assert_equal $values [r argetrange myarray 8388608 [expr {8388608 + $kmax - 1}]]
        assert_equal "old[expr {2 * ($kmax - 2)}]" [r arget myarray [expr {8388608 + 2 * ($kmax - 2)}]]
    }

    # =========================================================================
    # Range delete + iterator tests (dense→sparse demotion, superdir, sparse)
    # =========================================================================

    test {ARDELRANGE triggers dense to sparse demotion} {
        r del myarray
        # Pin config to ensure test doesn't break if defaults change
        set orig_kmin [lindex [r config get array-sparse-kmin] 1]
        r config set array-sparse-kmin 5

        # Create a dense slice with 50 elements
        for {set i 0} {$i < 50} {incr i} {
            r arset myarray $i "val$i"
        }
        assert_equal 50 [r arcount myarray]

        # Verify it's dense
        set info [r arinfo myarray FULL]
        assert_equal 1 [dict get $info dense-slices]
        assert_equal 0 [dict get $info sparse-slices]

        # Delete most elements with ARDELRANGE, leaving only 3 (below kmin=5)
        assert_equal 47 [r ardelrange myarray 3 49]
        assert_equal 3 [r arcount myarray]

        # Verify demotion to sparse
        set info [r arinfo myarray FULL]
        assert_equal 0 [dict get $info dense-slices]
        assert_equal 1 [dict get $info sparse-slices]

        # Verify remaining elements
        assert_equal "val0" [r arget myarray 0]
        assert_equal "val1" [r arget myarray 1]
        assert_equal "val2" [r arget myarray 2]

        r config set array-sparse-kmin $orig_kmin
    }

    test {ARDELRANGE partial delete preserves dense then demotes} {
        r del myarray
        # Pin config
        set orig_kmin [lindex [r config get array-sparse-kmin] 1]
        r config set array-sparse-kmin 5

        # Create dense slice
        for {set i 0} {$i < 40} {incr i} {
            r arset myarray $i $i
        }

        # Delete some but not enough to trigger demotion (keep 10 > kmin=5)
        assert_equal 30 [r ardelrange myarray 10 39]
        assert_equal 10 [r arcount myarray]

        set info [r arinfo myarray FULL]
        assert_equal 1 [dict get $info dense-slices]

        # Now delete more to trigger demotion
        assert_equal 6 [r ardelrange myarray 4 9]
        assert_equal 4 [r arcount myarray]

        set info [r arinfo myarray FULL]
        assert_equal 0 [dict get $info dense-slices]
        assert_equal 1 [dict get $info sparse-slices]

        r config set array-sparse-kmin $orig_kmin
    }

    test {ARDELRANGE deletes full slices within superdir block} {
        r del myarray
        # With slice_size=4096:
        # - Slice 2048 starts at index 8388608
        # - Slice 2049 starts at index 8392704
        # - Both are in superdir block 1

        # Create elements in two adjacent slices within same superdir block
        r arset myarray 8388608 "slice2048_a"
        r arset myarray 8388700 "slice2048_b"
        r arset myarray 8392704 "slice2049_a"
        r arset myarray 8392800 "slice2049_b"
        # And one element in a different block for reference
        r arset myarray 0 "slice0"

        assert_equal 5 [r arcount myarray]

        # Delete range that fully covers both slices 2048 and 2049
        # This should trigger full-slice deletion (not element-by-element)
        assert_equal 4 [r ardelrange myarray 8388608 8396799]
        assert_equal 1 [r arcount myarray]

        # Verify only slice0 element remains
        assert_equal "slice0" [r arget myarray 0]
        assert_equal {} [r arget myarray 8388608]
        assert_equal {} [r arget myarray 8392704]

        r del myarray
    }

    test {ARDELRANGE spanning multiple superdir blocks} {
        r del myarray
        # Superdir block boundaries with slice_size=4096:
        # - Block 0: slices 0-2047 (indices 0 - 8388607)
        # - Block 1: slices 2048-4095 (indices 8388608 - 16777215)
        # - Block 2: slices 4096+ (indices 16777216+)

        # Create elements across three blocks
        r arset myarray 100 "block0"
        r arset myarray 8388608 "block1_start"
        r arset myarray 12000000 "block1_mid"
        r arset myarray 16777200 "block1_end"
        r arset myarray 16777216 "block2_start"
        r arset myarray 20000000 "block2_mid"

        assert_equal 6 [r arcount myarray]

        # Delete range spanning from block1 into block2
        # This exercises cross-block deletion
        assert_equal 4 [r ardelrange myarray 8388608 18000000]
        assert_equal 2 [r arcount myarray]

        # Verify block0 and remaining block2 element
        assert_equal "block0" [r arget myarray 100]
        assert_equal "block2_mid" [r arget myarray 20000000]
        assert_equal {} [r arget myarray 8388608]
        assert_equal {} [r arget myarray 16777216]

        r del myarray
    }

    test {ARDELRANGE superdir middle range with missing upper block} {
        r del myarray
        # Occupied blocks:
        # - block 0: boundary lo_slice
        # - block 1: middle full slices to delete
        # - block 3: boundary hi_slice
        # block 2 is intentionally empty, so the upper lower-bound search
        # must stop at the insertion point rather than on an exact match.
        r arset myarray 8388590 "block0_keep"
        r arset myarray 8388608 "block1_a"
        r arset myarray 8392704 "block1_b"
        r arset myarray 25165825 "block3_keep"

        assert_equal 4 [r arcount myarray]
        assert_equal 2 [r ardelrange myarray 8388595 25165824]
        assert_equal 2 [r arcount myarray]

        assert_equal "block0_keep" [r arget myarray 8388590]
        assert_equal {} [r arget myarray 8388608]
        assert_equal {} [r arget myarray 8392704]
        assert_equal "block3_keep" [r arget myarray 25165825]
    }

    test {ARDELRANGE superdir with empty middle block interval} {
        r del myarray
        # Only the boundary slices are populated. The superdir middle interval
        # is empty, so the block loop must resolve to [start, end) = empty.
        r arset myarray 8388590 "block0_keep"
        r arset myarray 8388607 "block0_del"
        r arset myarray 25165824 "block3_del"
        r arset myarray 25165825 "block3_keep"

        assert_equal 4 [r arcount myarray]
        assert_equal 2 [r ardelrange myarray 8388600 25165824]
        assert_equal 2 [r arcount myarray]

        assert_equal "block0_keep" [r arget myarray 8388590]
        assert_equal {} [r arget myarray 8388607]
        assert_equal {} [r arget myarray 25165824]
        assert_equal "block3_keep" [r arget myarray 25165825]
    }

    test {ARDELRANGE with multiple ranges in single call} {
        r del myarray
        for {set i 0} {$i < 20} {incr i} {
            r arset myarray $i "val$i"
        }
        assert_equal 20 [r arcount myarray]

        # Delete two separate ranges in one command
        # Ranges: [2,4] and [10,14]
        assert_equal 8 [r ardelrange myarray 2 4 10 14]
        assert_equal 12 [r arcount myarray]

        # Verify correct elements deleted
        assert_equal "val0" [r arget myarray 0]
        assert_equal "val1" [r arget myarray 1]
        assert_equal {} [r arget myarray 2]
        assert_equal {} [r arget myarray 3]
        assert_equal {} [r arget myarray 4]
        assert_equal "val5" [r arget myarray 5]
        assert_equal "val9" [r arget myarray 9]
        assert_equal {} [r arget myarray 10]
        assert_equal {} [r arget myarray 14]
        assert_equal "val15" [r arget myarray 15]
    }

    test {ARDELRANGE with overlapping ranges} {
        r del myarray
        for {set i 0} {$i < 20} {incr i} {
            r arset myarray $i "val$i"
        }

        # Overlapping ranges: [5,12] and [8,15]
        # Should delete [5,15] total = 11 elements
        # But second range re-deletes already-deleted [8,12], so still 11 unique
        assert_equal 11 [r ardelrange myarray 5 12 8 15]
        assert_equal 9 [r arcount myarray]

        assert_equal "val4" [r arget myarray 4]
        assert_equal {} [r arget myarray 5]
        assert_equal {} [r arget myarray 12]
        assert_equal {} [r arget myarray 15]
        assert_equal "val16" [r arget myarray 16]
    }

    test {ARDELRANGE sparse slice middle-span deletion} {
        r del myarray
        # Create sparse slice with specific offsets
        r arset myarray 10 "a"
        r arset myarray 20 "b"
        r arset myarray 30 "c"
        r arset myarray 40 "d"
        r arset myarray 50 "e"

        assert_equal 5 [r arcount myarray]

        # Delete a middle contiguous sparse span.
        assert_equal 3 [r ardelrange myarray 20 40]
        assert_equal 2 [r arcount myarray]

        # Verify correct elements remain
        assert_equal "a" [r arget myarray 10]
        assert_equal {} [r arget myarray 20]
        assert_equal {} [r arget myarray 30]
        assert_equal {} [r arget myarray 40]
        assert_equal "e" [r arget myarray 50]
    }

    test {ARDELRANGE sparse with non-contiguous deletions} {
        r del myarray
        # Sparse elements at various offsets
        r arset myarray 5 "v5"
        r arset myarray 15 "v15"
        r arset myarray 25 "v25"
        r arset myarray 35 "v35"
        r arset myarray 45 "v45"

        # Delete range that only hits some elements
        assert_equal 2 [r ardelrange myarray 10 30]
        assert_equal 3 [r arcount myarray]

        assert_equal "v5" [r arget myarray 5]
        assert_equal {} [r arget myarray 15]
        assert_equal {} [r arget myarray 25]
        assert_equal "v35" [r arget myarray 35]
        assert_equal "v45" [r arget myarray 45]
    }

    test {ARDELRANGE sparse prefix span deletion} {
        r del myarray
        r arset myarray 10 "a"
        r arset myarray 20 "b"
        r arset myarray 30 "c"
        r arset myarray 40 "d"
        r arset myarray 50 "e"

        # Delete the sparse prefix span: first == 0, last in the middle.
        assert_equal 2 [r ardelrange myarray 0 25]
        assert_equal 3 [r arcount myarray]

        assert_equal {} [r arget myarray 10]
        assert_equal {} [r arget myarray 20]
        assert_equal "c" [r arget myarray 30]
        assert_equal "d" [r arget myarray 40]
        assert_equal "e" [r arget myarray 50]
    }

    test {ARDELRANGE sparse suffix span deletion} {
        r del myarray
        r arset myarray 10 "a"
        r arset myarray 20 "b"
        r arset myarray 30 "c"
        r arset myarray 40 "d"
        r arset myarray 50 "e"

        # Delete the sparse suffix span: first in the middle, last == count.
        assert_equal 2 [r ardelrange myarray 35 100]
        assert_equal 3 [r arcount myarray]

        assert_equal "a" [r arget myarray 10]
        assert_equal "b" [r arget myarray 20]
        assert_equal "c" [r arget myarray 30]
        assert_equal {} [r arget myarray 40]
        assert_equal {} [r arget myarray 50]
    }

    test {ARDELRANGE sparse whole-slice deletion} {
        r del myarray
        r arset myarray 10 "a"
        r arset myarray 20 "b"
        r arset myarray 30 "c"
        r arset myarray 40 "d"
        r arset myarray 50 "e"

        # Delete the whole sparse slice: first == 0, last == count.
        assert_equal 5 [r ardelrange myarray 0 100]
        assert_equal 0 [r exists myarray]
    }

    test {ARDELRANGE sparse no-hit range} {
        r del myarray
        r arset myarray 10 "a"
        r arset myarray 20 "b"
        r arset myarray 30 "c"
        r arset myarray 40 "d"
        r arset myarray 50 "e"

        # Delete a range that falls strictly between two sparse offsets.
        assert_equal 0 [r ardelrange myarray 11 19]
        assert_equal 5 [r arcount myarray]

        assert_equal "a" [r arget myarray 10]
        assert_equal "b" [r arget myarray 20]
        assert_equal "c" [r arget myarray 30]
        assert_equal "d" [r arget myarray 40]
        assert_equal "e" [r arget myarray 50]
    }

    test {ARDELRANGE sparse single edge deletions} {
        r del myarray
        r arset myarray 10 "a"
        r arset myarray 20 "b"
        r arset myarray 30 "c"
        r arset myarray 40 "d"
        r arset myarray 50 "e"

        # Delete exactly the first sparse element, then exactly the last one.
        assert_equal 1 [r ardelrange myarray 10 10]
        assert_equal 4 [r arcount myarray]
        assert_equal {} [r arget myarray 10]
        assert_equal "b" [r arget myarray 20]
        assert_equal "c" [r arget myarray 30]
        assert_equal "d" [r arget myarray 40]
        assert_equal "e" [r arget myarray 50]

        assert_equal 1 [r ardelrange myarray 50 50]
        assert_equal 3 [r arcount myarray]
        assert_equal "b" [r arget myarray 20]
        assert_equal "c" [r arget myarray 30]
        assert_equal "d" [r arget myarray 40]
        assert_equal {} [r arget myarray 50]
    }

    test {Random testing - blackbox ARDELRANGE model stress} {
        r flushdb
        expr {srand(24680)}
        array set model_state {}

        for {set step 0} {$step < 400} {incr step} {
            set roll [expr {int(rand() * 100)}]

            if {$roll < 50} {
                set idx [random_array_index]
                set val [random_value]
                r arset myarray $idx $val
                set model_state($idx) $val
            } elseif {$roll < 70} {
                set idx [random_array_index]
                set expected_deleted 0
                if {[info exists model_state($idx)]} {
                    unset model_state($idx)
                    set expected_deleted 1
                }
                assert_equal $expected_deleted [r ardel myarray $idx]
            } else {
                set args {}
                set expected_deleted 0
                set nranges [expr {int(rand() * 3) + 1}]

                for {set i 0} {$i < $nranges} {incr i} {
                    set lo [random_array_index]
                    set hi [random_array_index]
                    lappend args $lo $hi
                    incr expected_deleted [model_array_delrange model_state $lo $hi]
                }

                assert_equal $expected_deleted [r ardelrange myarray {*}$args]
            }

            if {$step % 25 == 0 || $step == 399} {
                set expected_scan [model_array_scan model_state]
                set expected_count [array size model_state]

                if {$expected_count == 0} {
                    assert_equal 0 [r exists myarray]
                    assert_equal {} [r arscan myarray 0 30000000]
                } else {
                    assert_equal $expected_count [r arcount myarray]
                    assert_equal $expected_scan [r arscan myarray 0 30000000]
                }

                for {set probe 0} {$probe < 20} {incr probe} {
                    set idx [random_array_index]
                    if {[info exists model_state($idx)]} {
                        assert_equal $model_state($idx) [r arget myarray $idx]
                    } else {
                        assert_equal {} [r arget myarray $idx]
                    }
                }
            }
        }
    }

    test {ARSCAN after ARDELRANGE with demotion} {
        r del myarray
        # Create dense
        for {set i 0} {$i < 30} {incr i} {
            r arset myarray $i "val$i"
        }

        # Delete most, triggering demotion
        r ardelrange myarray 4 29

        # ARSCAN should find remaining elements
        set result [r arscan myarray 0 100]
        assert_equal 4 [llength $result]
        assert_equal {{0 val0} {1 val1} {2 val2} {3 val3}} $result

        # Reverse scan
        set result [r arscan myarray 100 0]
        assert_equal {{3 val3} {2 val2} {1 val1} {0 val0}} $result
    }

    test {ARSCAN with LIMIT after range delete} {
        r del myarray
        for {set i 0} {$i < 20} {incr i} {
            r arset myarray $i $i
        }

        # Delete some in the middle
        r ardelrange myarray 5 14

        # Scan with limit
        set result [r arscan myarray 0 100 LIMIT 3]
        assert_equal 3 [llength $result]
        assert_equal {{0 0} {1 1} {2 2}} $result
    }

    test {AROP after ARDELRANGE across multiple slices} {
        r del myarray
        # Create elements across slice boundaries (slice_size=4096)
        for {set i 0} {$i < 10} {incr i} {
            r arset myarray $i $i
        }
        for {set i 4096} {$i < 4106} {incr i} {
            r arset myarray $i $i
        }

        assert_equal 20 [r arcount myarray]

        # Delete first slice partially
        r ardelrange myarray 5 9

        # AROP SUM should work across slices
        # Remaining: 0+1+2+3+4 + 4096..4105 = 10 + sum(4096..4105)
        # sum(4096..4105) = (4096+4105)*10/2 = 41005
        set sum [r arop myarray 0 5000 SUM]
        assert_equal 41015 $sum

        # AROP USED
        assert_equal 15 [r arop myarray 0 5000 USED]

        # AROP MIN/MAX
        assert_equal 0 [r arop myarray 0 5000 MIN]
        assert_equal 4105 [r arop myarray 0 5000 MAX]
    }

    test {AROP MATCH after dense demotion} {
        r del myarray
        # Create dense with repeated values
        for {set i 0} {$i < 30} {incr i} {
            r arset myarray $i "target"
        }
        r arset myarray 2 "other"

        # Delete most to trigger demotion, keep indices 0-3
        # After delete: 0=target, 1=target, 2=other, 3=target
        r ardelrange myarray 4 29

        # Verify demotion happened
        set info [r arinfo myarray FULL]
        assert_equal 0 [dict get $info dense-slices]
        assert_equal 1 [dict get $info sparse-slices]

        # Count matches in sparse slice (3 "target" values)
        assert_equal 3 [r arop myarray 0 100 MATCH target]
    }

    test {ARSCAN over superdir blocks} {
        r del myarray
        # Elements in different superdir blocks
        r arset myarray 0 "first"
        r arset myarray 8388608 "second"
        r arset myarray 16777216 "third"

        # Scan entire range
        set result [r arscan myarray 0 20000000]
        assert_equal 3 [llength $result]
        assert_equal {0 first} [lindex $result 0]
        assert_equal {8388608 second} [lindex $result 1]
        assert_equal {16777216 third} [lindex $result 2]

        # Reverse scan
        set result [r arscan myarray 20000000 0]
        assert_equal {16777216 third} [lindex $result 0]
        assert_equal {8388608 second} [lindex $result 1]
        assert_equal {0 first} [lindex $result 2]

        r del myarray
    }

    test {Iterator commands do not rescan exhausted superdir blocks} {
        r del myarray
        r arset myarray 43 "a"
        r arset myarray 4586 "b"
        r arset myarray 19245258 "c"

        assert_equal {{43 a} {4586 b} {19245258 c}} \
            [r arscan myarray 0 30000000 LIMIT 8]
        assert_equal {{19245258 c}} \
            [r argrep myarray 0 30000000 EXACT c WITHVALUES LIMIT 4]
        assert_equal 3 [r arop myarray 0 30000000 USED]
    }

    test {AROP over superdir with partial range} {
        r del myarray
        r arset myarray 0 10
        r arset myarray 100 20
        r arset myarray 8388608 30
        r arset myarray 8388700 40
        r arset myarray 16777216 50

        # SUM only in first block
        assert_equal 30 [r arop myarray 0 1000 SUM]

        # SUM spanning blocks
        assert_equal 150 [r arop myarray 0 20000000 SUM]

        # USED in specific range
        assert_equal 2 [r arop myarray 8388600 8388800 USED]

        r del myarray
    }

    test {ARDELRANGE delete entire slice then verify iteration} {
        r del myarray
        # Two slices
        for {set i 0} {$i < 10} {incr i} {
            r arset myarray $i "slice0_$i"
        }
        for {set i 4096} {$i < 4106} {incr i} {
            r arset myarray $i "slice1_$i"
        }

        # Delete entire first slice
        assert_equal 10 [r ardelrange myarray 0 4095]
        assert_equal 10 [r arcount myarray]

        # ARSCAN should only find second slice elements
        set result [r arscan myarray 0 5000]
        assert_equal 10 [llength $result]
        assert_equal {4096 slice1_4096} [lindex $result 0]
    }

}

# Test loading a 32-bit generated RDB on the current architecture.
# The RDB file contains arrays exercising all tagged pointer encodings:
# immediate ints (including 30-bit boundary values), inline floats,
# small strings, arString heap strings, mixed types, sparse indices,
# and insert_idx preservation.
set server_path [tmpdir "server.array-32bit-rdb-test"]
exec cp [file join [pwd] tests/assets/array-32bit.rdb] $server_path

start_server [list overrides [list "dir" $server_path "dbfilename" "array-32bit.rdb"] tags {"array external:skip"}] {

    test {Load 32-bit RDB - integer encodings} {
        r select 0
        # Inline ints and boundary values
        assert_equal 0             [r arget ints 0]
        assert_equal 1             [r arget ints 1]
        assert_equal -1            [r arget ints 2]
        assert_equal 42            [r arget ints 3]
        assert_equal -42           [r arget ints 4]
        # 30-bit int boundary (max/min for 32-bit tagged ints)
        assert_equal 536870911     [r arget ints 5]
        assert_equal -536870912    [r arget ints 6]
        # Values beyond 30-bit range (arString on 32-bit, re-encoded on load)
        assert_equal 536870912     [r arget ints 7]
        assert_equal -536870913    [r arget ints 8]
        assert_equal 2147483647    [r arget ints 9]
        assert_equal -2147483648   [r arget ints 10]
        assert_equal 1000000000    [r arget ints 11]
        assert_equal 999999999     [r arget ints 12]
        assert_equal 100           [r arget ints 13]
        assert_equal 14            [r arcount ints]
    }

    test {Load 32-bit RDB - float encodings} {
        r select 0
        assert_equal 1.0           [r arget floats 0]
        assert_equal -1.0          [r arget floats 1]
        assert_equal 3.14          [r arget floats 2]
        assert_equal 0.5           [r arget floats 3]
        assert_equal -0.5          [r arget floats 4]
        assert_equal 0.25          [r arget floats 5]
        assert_equal 100.0         [r arget floats 6]
        assert_equal -100.0        [r arget floats 7]
        assert_equal 1.5           [r arget floats 8]
        assert_equal 1.75          [r arget floats 9]
        assert_equal 0.1           [r arget floats 10]
        assert_equal 1234.5        [r arget floats 11]
        assert_equal 0.0625        [r arget floats 12]
        assert_equal 999999.0      [r arget floats 13]
        assert_equal 1.23456789012 [r arget floats 14]
        assert_equal 15            [r arcount floats]
    }

    test {Load 32-bit RDB - string encodings} {
        r select 0
        # Empty string, 1-3 byte inline (smallstr on 32-bit),
        # 4-7 byte (smallstr on 64-bit only, arString on 32-bit),
        # 8+ byte (always arString)
        assert_equal {}                                  [r arget strs 0]
        assert_equal a                                   [r arget strs 1]
        assert_equal ab                                  [r arget strs 2]
        assert_equal abc                                 [r arget strs 3]
        assert_equal abcd                                [r arget strs 4]
        assert_equal abcde                               [r arget strs 5]
        assert_equal abcdef                              [r arget strs 6]
        assert_equal abcdefg                             [r arget strs 7]
        assert_equal abcdefgh                            [r arget strs 8]
        assert_equal {hello world}                       [r arget strs 9]
        assert_equal {this is a longer string for testing} [r arget strs 10]
        assert_equal x                                   [r arget strs 11]
        assert_equal xy                                  [r arget strs 12]
        assert_equal xyz                                 [r arget strs 13]
        assert_equal 14                                  [r arcount strs]
    }

    test {Load 32-bit RDB - mixed type encodings} {
        r select 0
        assert_equal 42            [r arget mixed 0]
        assert_equal 3.14          [r arget mixed 1]
        assert_equal hi            [r arget mixed 2]
        assert_equal -536870912    [r arget mixed 3]
        assert_equal 0.5           [r arget mixed 4]
        assert_equal abcdefghij    [r arget mixed 5]
        assert_equal 536870911     [r arget mixed 6]
        assert_equal -1.5          [r arget mixed 7]
        assert_equal ab            [r arget mixed 8]
        assert_equal 0             [r arget mixed 9]
        assert_equal 1.0           [r arget mixed 10]
        assert_equal hello         [r arget mixed 11]
        assert_equal 2147483647    [r arget mixed 12]
        assert_equal 0.25          [r arget mixed 13]
        assert_equal xyz           [r arget mixed 14]
        assert_equal 15            [r arcount mixed]
    }

    test {Load 32-bit RDB - sparse indices across slices} {
        r select 0
        assert_equal first       [r arget sparse 0]
        assert_equal slice0end   [r arget sparse 4095]
        assert_equal slice1start [r arget sparse 4096]
        assert_equal slice1end   [r arget sparse 8191]
        assert_equal 42          [r arget sparse 10000]
        assert_equal 3.14        [r arget sparse 50000]
        assert_equal hello       [r arget sparse 100000]
        assert_equal 7           [r arcount sparse]
    }

    test {Load 32-bit RDB - insert_idx preservation} {
        r select 0
        assert_equal one   [r arget withinsert 0]
        assert_equal two   [r arget withinsert 1]
        assert_equal three [r arget withinsert 2]
        assert_equal four  [r arget withinsert 3]
        assert_equal five  [r arget withinsert 4]
        assert_equal 5     [r arcount withinsert]
        # Verify insert_idx was preserved: next insert should go at index 5
        r arinsert withinsert six
        assert_equal six   [r arget withinsert 5]
    }

    test {Load 32-bit RDB - re-save and reload cycle} {
        r select 0
        # Save from 64-bit, reload, verify integrity
        r save
        r debug reload
        foreach {idx value} {
            0 0 1 1 2 -1 3 42 4 -42
            5 536870911 6 -536870912 7 536870912 8 -536870913
            9 2147483647 10 -2147483648 11 1000000000 12 999999999 13 100
        } {
            assert_equal $value [r arget ints $idx]
        }
        assert_equal 14 [r arcount ints]

        foreach {idx value} {
            0 1.0 1 -1.0 2 3.14 3 0.5 4 -0.5
            5 0.25 6 100.0 7 -100.0 8 1.5 9 1.75
            10 0.1 11 1234.5 12 0.0625 13 999999.0 14 1.23456789012
        } {
            assert_equal $value [r arget floats $idx]
        }
        assert_equal 15 [r arcount floats]

        foreach {idx value} {
            0 {} 1 a 2 ab 3 abc 4 abcd 5 abcde 6 abcdef 7 abcdefg
            8 abcdefgh 9 {hello world} 10 {this is a longer string for testing}
            11 x 12 xy 13 xyz
        } {
            assert_equal $value [r arget strs $idx]
        }
        assert_equal 14 [r arcount strs]

        foreach {idx value} {
            0 42 1 3.14 2 hi 3 -536870912 4 0.5
            5 abcdefghij 6 536870911 7 -1.5 8 ab 9 0
            10 1.0 11 hello 12 2147483647 13 0.25 14 xyz
        } {
            assert_equal $value [r arget mixed $idx]
        }
        assert_equal 15 [r arcount mixed]

        foreach {idx value} {
            0 first 4095 slice0end 4096 slice1start 8191 slice1end
            10000 42 50000 3.14 100000 hello
        } {
            assert_equal $value [r arget sparse $idx]
        }
        assert_equal 7 [r arcount sparse]

        foreach {idx value} {
            0 one 1 two 2 three 3 four 4 five 5 six
        } {
            assert_equal $value [r arget withinsert $idx]
        }
        assert_equal 6 [r arcount withinsert]
        r arinsert withinsert seven
        assert_equal seven [r arget withinsert 6]
    } {} {needs:debug}
}
