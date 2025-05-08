set testmodule [file normalize tests/modules/hash.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test {Module hash set} {
        r set k mystring
        assert_error "WRONGTYPE*" {r hash.set k "" hello world}
        r del k
        # "" = count updates and deletes of existing fields only
        assert_equal 0 [r hash.set k "" squirrel yes]
        # "a" = COUNT_ALL = count inserted, modified and deleted fields
        assert_equal 2 [r hash.set k "a" banana no sushi whynot]
        # "n" = NX = only add fields not already existing in the hash
        # "x" = XX = only replace the value for existing fields
        assert_equal 0 [r hash.set k "n" squirrel hoho what nothing]
        assert_equal 1 [r hash.set k "na" squirrel hoho something nice]
        assert_equal 0 [r hash.set k "xa" new stuff not inserted]
        assert_equal 1 [r hash.set k "x" squirrel ofcourse]
        assert_equal 1 [r hash.set k "" sushi :delete: none :delete:]
        r hgetall k
    } {squirrel ofcourse banana no what nothing something nice}

    test {Module hash - set (override) NX expired field successfully} {
        r debug set-active-expire 0
        r del H1 H2
        r hash.set H1 "n" f1 v1
        r hpexpire H1 1 FIELDS 1 f1
        r hash.set H2 "n" f1 v1 f2 v2
        r hpexpire H2 1 FIELDS 1 f1
        after 5
        assert_equal 0 [r hash.set H1 "n" f1 xx]
        assert_equal "f1 xx" [r hgetall H1]
        assert_equal 0 [r hash.set H2 "n" f1 yy]
        assert_equal "f1 f2 v2 yy" [lsort [r hgetall H2]]
        r debug set-active-expire 1
    } {OK} {needs:debug}

    test {Module hash - set XX of expired field gets failed as expected} {
        r debug set-active-expire 0
        r del H1 H2
        r hash.set H1 "n" f1 v1
        r hpexpire H1 1 FIELDS 1 f1
        r hash.set H2 "n" f1 v1 f2 v2
        r hpexpire H2 1 FIELDS 1 f1
        after 5

        # expected to fail on condition XX. hgetall should return empty list
        r hash.set H1 "x" f1 xx
        assert_equal "" [lsort [r hgetall H1]]
        # But expired field was not lazy deleted
        assert_equal 1 [r hlen H1]

        # expected to fail on condition XX. hgetall should return list without expired f1
        r hash.set H2 "x" f1 yy
        assert_equal "f2 v2" [lsort [r hgetall H2]]
        # But expired field was not lazy deleted
        assert_equal 2 [r hlen H2]

        r debug set-active-expire 1
    } {OK} {needs:debug}

    test {Module hash - test open key with REDISMODULE_OPEN_KEY_ACCESS_EXPIRED to scan expired fields} {
        r debug set-active-expire 0
        r del H1
        r hash.set H1 "n" f1 v1 f2 v2 f3 v3
        r hpexpire H1 1 FIELDS 2 f1 f2
        after 10
        # Scan expired fields with flag REDISMODULE_OPEN_KEY_ACCESS_EXPIRED
        assert_equal "f1 f2 f3 v1 v2 v3" [lsort [r hash.hscan_expired H1]]
        # Get expired field with flag REDISMODULE_OPEN_KEY_ACCESS_EXPIRED
        assert_equal {v1} [r hash.hget_expired H1 f1]
        # Verify we can get the TTL of the expired field as well
        set now [expr [clock seconds]*1000]
        assert_range [r hash.hget_expire H1 f2] [expr {$now-1000}] [expr {$now+1000}]        
        # Verify key doesn't exist on normal access without the flag
        assert_equal 0 [r hexists H1 f1]
        assert_equal 0 [r hexists H1 f2]
        # Scan again expired fields with flag REDISMODULE_OPEN_KEY_ACCESS_EXPIRED
        assert_equal "f3 v3" [lsort [r hash.hscan_expired H1]]
        r debug set-active-expire 1
    }

    test {Module hash - test open key with REDISMODULE_OPEN_KEY_ACCESS_EXPIRED to scan expired key} {
        r debug set-active-expire 0
        r del H1
        r hash.set H1 "n" f1 v1 f2 v2 f3 v3
        r pexpire H1 5
        after 10
        # Scan expired fields with flag REDISMODULE_OPEN_KEY_ACCESS_EXPIRED
        assert_equal "f1 f2 f3 v1 v2 v3" [lsort [r hash.hscan_expired H1]]
        # Get expired field with flag REDISMODULE_OPEN_KEY_ACCESS_EXPIRED
        assert_equal {v1} [r hash.hget_expired H1 f1]
        # Verify key doesn't exist on normal access without the flag
        assert_equal 0 [r exists H1]
        r debug set-active-expire 1
    }
    
    test {Module hash - Read field expiration time} {
        r del H1
        r hash.set H1 "n" f1 v1 f2 v2 f3 v3 f4 v4
        r hexpire H1 10   FIELDS 1 f1
        r hexpire H1 100  FIELDS 1 f2
        r hexpire H1 1000 FIELDS 1 f3        
        
        # Validate that the expiration times for fields f1, f2, and f3 are correct
        set nowMsec [expr [clock seconds]*1000]
        assert_range [r hash.hget_expire H1 f1] [expr {$nowMsec+9000}] [expr {$nowMsec+11000}]
        assert_range [r hash.hget_expire H1 f2] [expr {$nowMsec+90000}] [expr {$nowMsec+110000}]
        assert_range [r hash.hget_expire H1 f3] [expr {$nowMsec+900000}] [expr {$nowMsec+1100000}]
        
        # Assert that field f4 and f5_not_exist have no expiration (should return -1)
        assert_equal [r hash.hget_expire H1 f4] -1  
        assert_equal [r hash.hget_expire H1 f5_not_exist] -1
        
        # Assert that variadic version of hget_expire works as well
        assert_equal [r hash.hget_two_expire H1 f1 f2] [list [r hash.hget_expire H1 f1] [r hash.hget_expire H1 f2]]        
    }
    
    test {Module hash - Read minimum expiration time} {
        r del H1
        r hash.set H1 "n" f1 v1 f2 v2 f3 v3 f4 v4
        r hexpire H1 100   FIELDS 1 f1
        r hexpire H1 10    FIELDS 1 f2
        r hexpire H1 1000  FIELDS 1 f3        
        
        # Validate that the minimum expiration time is correct
        set nowMsec [expr [clock seconds]*1000]
        assert_range [r hash.hget_min_expire H1] [expr {$nowMsec+9000}] [expr {$nowMsec+11000}]
        assert_equal [r hash.hget_min_expire H1] [r hash.hget_expire H1 f2]
        
        # Assert error if key not found
        assert_error {*key not found*} {r hash.hget_min_expire non_exist_hash}
        
        # Assert return -1 if no expiration (=REDISMODULE_NO_EXPIRE)
        r del H2
        r hash.set H2 "n" f1 v1
        assert_equal [r hash.hget_min_expire H2] -1
    }

    test {Module hash - KEYSIZES is updated as expected} {
        proc run_cmd_verify_hist {cmd expOutput {retries 1}} {
            proc K {} {return [string map { "db0_distrib_hashes_items" "db0_HASH" "# Keysizes" "" " " "" "\n" "" "\r" "" } [r info keysizes] ]}
            uplevel 1 $cmd    
            wait_for_condition 50 $retries {
                $expOutput eq [K]
            } else { fail "Expected: \n`$expOutput`\n Actual:\n`[K]`.\nFailed after command: $cmd" }
        }
        
        r select 0
        r flushall
        # Check RM_HashSet 
        run_cmd_verify_hist {r hash.set H1 "n" f1 v1} {db0_HASH:1=1}
        run_cmd_verify_hist {r hash.set H2 "n" f1 v1} {db0_HASH:1=2}
        run_cmd_verify_hist {r hash.set H2 "n" f1 v1} {db0_HASH:1=2}
        run_cmd_verify_hist {r hash.set H2 "x" f1 v1} {db0_HASH:1=2}
        run_cmd_verify_hist {r hash.set H3 "x" f1 v1} {db0_HASH:1=2}
        run_cmd_verify_hist {r hash.set H1 "n" f2 v2} {db0_HASH:1=1,2=1}
        run_cmd_verify_hist {r hash.set H1 "a" f3 v3 f4 v4} {db0_HASH:1=1,4=1}
        run_cmd_verify_hist {r del H1} {db0_HASH:1=1}
        run_cmd_verify_hist {r del H2} {}        
        
        # Check lazy expire
        r debug set-active-expire 0
        run_cmd_verify_hist {r hash.set H1 "n" f1 v1} {db0_HASH:1=1}
        run_cmd_verify_hist {r hpexpire H1 1 FIELDS 1 f1} {db0_HASH:1=1}
        run_cmd_verify_hist {after 5} {db0_HASH:1=1}
        run_cmd_verify_hist {r hash.hget_expired H1 f1} {db0_HASH:1=1}
        r debug set-active-expire 1
        run_cmd_verify_hist {after 5} {} 50
    }

    test "Unload the module - hash" {
        assert_equal {OK} [r module unload hash]
    }
}
