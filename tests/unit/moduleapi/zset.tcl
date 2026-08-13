set testmodule [file normalize tests/modules/zset.so]

start_server {tags {"modules external:skip"}} {
    r module load $testmodule

    test {Module zset rem} {
        r del k
        r zadd k 100 hello 200 world
        assert_equal 1 [r zset.rem k hello]
        assert_equal 0 [r zset.rem k hello]
        assert_equal 1 [r exists k]
        # Check that removing the last element deletes the key
        assert_equal 1 [r zset.rem k world]
        assert_equal 0 [r exists k]
    }

    test {Module zset add} {
        r del k
        # Check that failure does not create empty key
        assert_error "ERR ZsetAdd failed" {r zset.add k nan hello}
        assert_equal 0 [r exists k]

        r zset.add k 100 hello
        assert_equal {hello 100} [r zrange k 0 -1 withscores]
    }

    test {Module zset incrby} {
        r del k
        # Check that failure does not create empty key
        assert_error "ERR ZsetIncrby failed" {r zset.incrby k hello nan}
        assert_equal 0 [r exists k]

        r zset.incrby k hello 100
        assert_equal {hello 100} [r zrange k 0 -1 withscores]
    }

    test {Module zset - KEYSIZES is updated as expected (like test at hash.tcl)} {
        proc run_cmd_verify_hist {cmd expOutput {retries 1}} {
            proc K {} {return [string map { "db0_distrib_zsets_items" "db0_ZSET" "# Keysizes" "" " " "" "\n" "" "\r" "" } [r info keysizes] ]}
            uplevel 1 $cmd    
            wait_for_condition 50 $retries {
                $expOutput eq [K]
            } else { fail "Expected: \n`$expOutput`\n Actual:\n`[K]`.\nFailed after command: $cmd" }
        }
        
        r select 0
        
        #RedisModule_ZsetAdd, RedisModule_ZsetRem
        run_cmd_verify_hist {r FLUSHALL} {}
        run_cmd_verify_hist {r zset.add k 100 hello} {db0_ZSET:1=1}
        run_cmd_verify_hist {r zset.add k 101 bye} {db0_ZSET:2=1}
        run_cmd_verify_hist {r zset.rem k hello} {db0_ZSET:1=1}
        run_cmd_verify_hist {r zset.rem k bye} {}
        
        #RM_ZsetIncrby
        run_cmd_verify_hist {r FLUSHALL} {}
        run_cmd_verify_hist {r zset.incrby k hello 100} {db0_ZSET:1=1}
        run_cmd_verify_hist {r zset.incrby k hello 100} {db0_ZSET:1=1}
        run_cmd_verify_hist {r zset.rem k hello} {}

        # Check lazy expire
        r debug set-active-expire 0
        run_cmd_verify_hist {r zset.add k 100 hello} {db0_ZSET:1=1}
        run_cmd_verify_hist {r pexpire k 2} {db0_ZSET:1=1}
        run_cmd_verify_hist {after 5} {db0_ZSET:1=1}
        r debug set-active-expire 1
        run_cmd_verify_hist {after 5} {} 50
    }

    test {Module zset DELALL functionality} {
        # Clean up any existing keys
        r flushall

        # Create some zsets and other types of keys
        r zadd zset1 100 hello 200 world
        r zadd zset2 300 foo 400 bar
        r zadd zset3 500 baz
        r set string1 "value1"
        r hset hash1 field1 value1
        r lpush list1 item1

        # Verify we have the expected keys
        assert_equal 6 [r dbsize]
        assert_equal 3 [llength [r keys zset*]]

        # Run zset.delall
        set deleted [r zset.delall]
        assert_equal 3 $deleted

        # Verify only zsets were deleted
        assert_equal 3 [r dbsize]
        assert_equal 0 [llength [r keys zset*]]
        assert_equal 1 [r exists string1]
        assert_equal 1 [r exists hash1]
        assert_equal 1 [r exists list1]

        # Test with no zsets
        set deleted [r zset.delall]
        assert_equal 0 $deleted
        assert_equal 3 [r dbsize]
    }

    test {Module zset DELALL not in transaction} {
        set repl [attach_to_replication_stream]
        r zadd z1 1 e1
        r zadd z2 1 e1
        r zset.delall
        assert_replication_stream $repl {
            {select *}
            {zadd z1 1 e1}
            {zadd z2 1 e1}
            {del z*}
            {del z*}
        }
        close_replication_stream $repl
    } {} {needs:repl}

    foreach {enc setup} {
        listpack {r config set zset-max-listpack-entries 128}
        btree    {r config set zset-max-listpack-entries 0}
    } {
        test "Module zset score range iterator - $enc" {
            eval $setup
            r del zk
            r zadd zk 1 a 2 b 3 c 4 d 5 e
            assert_encoding $enc zk
            assert_equal {b c d} [r zset.rangebyscore zk 2 4 asc]
            assert_equal {d c b} [r zset.rangebyscore zk 2 4 desc]
            # Full range walks the whole set with O(1) stepping.
            assert_equal {a b c d e} [r zset.rangebyscore zk -inf +inf asc]
            assert_equal {e d c b a} [r zset.rangebyscore zk -inf +inf desc]
            # Empty range.
            assert_equal {} [r zset.rangebyscore zk 100 200 asc]
        }

        test "Module zset lex range iterator - $enc" {
            eval $setup
            r del zk
            r zadd zk 0 a 0 b 0 c 0 d 0 e
            assert_encoding $enc zk
            assert_equal {b c d} [r zset.rangebylex zk {[b} {[d} asc]
            assert_equal {d c b} [r zset.rangebylex zk {[b} {[d} desc]
            assert_equal {a b c d e} [r zset.rangebylex zk - + asc]
            assert_equal {e d c b a} [r zset.rangebylex zk - + desc]
            assert_equal {c d e} [r zset.rangebylex zk {(b} + asc]
        }
    }

    test "Module zset range iterator large - btree" {
        r config set zset-max-listpack-entries 0
        r del zk
        for {set i 0} {$i < 1000} {incr i} {
            r zadd zk $i [format m%04d $i]
        }
        assert_encoding btree zk
        set expected {}
        for {set i 100} {$i <= 199} {incr i} {
            lappend expected [format m%04d $i]
        }
        # A 100-element sub-range walked purely via the O(1) persistent iterator.
        assert_equal $expected [r zset.rangebyscore zk 100 199 asc]
        assert_equal [lreverse $expected] [r zset.rangebyscore zk 100 199 desc]
    }
    r config set zset-max-listpack-entries 128

    test "Unload the module - zset" {
        assert_equal {OK} [r module unload zset]
    }
}
