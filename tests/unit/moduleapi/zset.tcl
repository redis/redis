set testmodule [file normalize tests/modules/zset.so]

start_server {tags {"modules"}} {
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
    
    test "Unload the module - zset" {
        assert_equal {OK} [r module unload zset]
    }
}
