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

    test {Module zset range iteration with btree encoding} {
        set old_max_entries [lindex [r config get zset-max-listpack-entries] 1]
        r config set zset-max-listpack-entries 0

        r del k
        r zadd k 1 a 2 b 2 c 3 d 4 e
        assert_encoding btree k
        assert_equal {d 3 e 4} [r zset.range k score first 2 4 1 0]
        assert_equal {e 4 d 3} [r zset.range k score last 2 4 1 0]
        assert_equal {} [r zset.range k score first 10 20 0 0]

        r del k
        r zadd k 0 alpha 0 beta 0 delta 0 gamma
        assert_encoding btree k
        assert_equal {beta 0 delta 0} \
            [r zset.range k lex first \[beta \(gamma]
        assert_equal {delta 0 beta 0} \
            [r zset.range k lex last \[beta \(gamma]
        assert_equal {} [r zset.range k lex first \[z +]

        r config set zset-max-listpack-entries $old_max_entries
    }

    test {Module zset range iteration ends after its saved rank is invalidated} {
        set old_max_entries [lindex [r config get zset-max-listpack-entries] 1]
        r config set zset-max-listpack-entries 0

        r del k
        r zadd k 1 a 2 b 3 c
        assert_encoding btree k
        assert_equal OK [r zset.range-mutate k]
        assert_equal {c} [r zrange k 0 -1]

        r config set zset-max-listpack-entries $old_max_entries
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

    test "Unload the module - zset" {
        assert_equal {OK} [r module unload zset]
    }
}
