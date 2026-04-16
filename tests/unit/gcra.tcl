start_server {tags {"gcra" "external:skip"}} {
    test {GCRA - argument validation} {
        # Wrong number of arguments (too few)
        catch {r gcra} err
        assert_match "*wrong number of arguments*" $err
        catch {r gcra mykey} err
        assert_match "*wrong number of arguments*" $err
        catch {r gcra mykey 10} err
        assert_match "*wrong number of arguments*" $err
        catch {r gcra mykey 10 5} err
        assert_match "*wrong number of arguments*" $err

        # max_burst must be non-negative integer
        catch {r gcra mykey -1 5 10} err
        assert_match "*out of range*" $err
        catch {r gcra mykey notanumber 5 10} err
        assert_match "*out of range*" $err

        # tokens_per_period must be >= 1
        catch {r gcra mykey 10 0 10} err
        assert_match "*out of range*" $err
        catch {r gcra mykey 10 -1 10} err
        assert_match "*out of range*" $err
        catch {r gcra mykey 10 notanumber 10} err
        assert_match "*not an integer*" $err

        # period must be positive
        catch {r gcra mykey 10 5 0} err
        assert_match "*period must be > 0*" $err
        catch {r gcra mykey 10 5 -1} err
        assert_match "*period must be > 0*" $err
        catch {r gcra mykey 10 5 notanumber} err
        assert_match "*not a valid float*" $err

        # tokens (optional) must be >= 1
        catch {r gcra mykey 10 5 10 TOKENS} err
        assert_match "*Missing TOKENS value*" $err
        catch {r gcra mykey 10 5 10 TOKENS 0} err
        assert_match "*out of range*" $err
        catch {r gcra mykey 10 5 10 TOKENS -1} err
        assert_match "*out of range*" $err
        catch {r gcra mykey 10 5 10 TOKENS notanumber} err
        assert_match "*not an integer*" $err

        # Valid arguments with default tokens
        r del mykey
        set result [r gcra mykey 10 5 60]
        assert_equal 5 [llength $result]
        set limited [lindex $result 0]
        set max_req_num [lindex $result 1]
        assert_equal $limited 0
        assert_equal 11 $max_req_num

        # Valid arguments with explicit tokens
        r del mykey
        set result [r gcra mykey 10 5 60 TOKENS 2]
        assert_equal 5 [llength $result]
        assert_equal 11 [lindex $result 1]

        # Period accepts fractional seconds
        r del mykey
        set result [r gcra mykey 10 5 0.5]
        assert_equal 5 [llength $result]
    }

    test {GCRA - first request is allowed} {
        r del mykey
        set result [r gcra mykey 5 10 60]
        set limited [lindex $result 0]
        # First request should not be limited
        assert_equal 0 $limited
    }

    test {GCRA - requests within burst are allowed} {
        r del mykey
        # max_burst=5, tokens_per_period=1, period=60
        # This allows burst of 6 requests (max_burst + 1)
        for {set i 0} {$i < 6} {incr i} {
            set result [r gcra mykey 5 1 60]
            set limited [lindex $result 0]
            assert_equal 0 $limited "Request $i should be allowed"
        }
        # Request 6 should be limited
        set result [r gcra mykey 5 1 60]
        set limited [lindex $result 0]
        assert_equal 1 $limited "Request 6 should be limited"
    }

    test {GCRA - retry_after is positive when limited} {
        r del mykey
        # Exhaust burst
        for {set i 0} {$i < 3} {incr i} {
            r gcra mykey 2 1 60
        }
        # Next request should be limited with positive retry_after
        set result [r gcra mykey 2 1 60]
        set limited [lindex $result 0]
        set retry_after [lindex $result 3]
        assert_equal 1 $limited
        assert {$retry_after > 0}
    }

    test {GCRA - retry_after is negative when allowed} {
        r del mykey
        set result [r gcra mykey 5 1 60]
        set limited [lindex $result 0]
        set retry_after [lindex $result 3]
        assert_equal 0 $limited
        assert {$retry_after < 0}
    }

    test {GCRA - num_avail_req decreases with each request} {
        r del mykey
        set result1 [r gcra mykey 5 1 60]
        set avail1 [lindex $result1 2]

        set result2 [r gcra mykey 5 1 60]
        set avail2 [lindex $result2 2]

        assert {$avail2 < $avail1}
    }

    test {GCRA - multiple tokens consumed per request} {
        r del mykey
        # max_burst=5, so 6 tokens available initially
        # Consume 1 token (default)
        set result1 [r gcra mykey 5 1 60]
        set avail1 [lindex $result1 2]
        assert_equal 5 $avail1

        r del mykey
        # Consume 3 tokens from fresh state
        set result2 [r gcra mykey 5 1 60 TOKENS 3]
        set avail2 [lindex $result2 2]
        assert_equal 3 $avail2
    }

    test {GCRA - rate recovery over time} {
        r del mykey
        # max_burst=1, tokens_per_period=1, period=1 (1 token per second)
        # Exhaust: 2 allowed (burst+1), then limited
        r gcra mykey 1 1 1
        r gcra mykey 1 1 1
        set result [r gcra mykey 1 1 1]
        assert_equal 1 [lindex $result 0] "Should be limited"

        # Wait for rate to recover
        after 1100

        # Should be allowed again
        set result [r gcra mykey 1 1 1]
        assert_equal 0 [lindex $result 0] "Should be allowed after recovery"
    }

    test {GCRA - full_burst_after indicates time to full recovery} {
        r del mykey
        # Consume some tokens
        r gcra mykey 5 1 60
        r gcra mykey 5 1 60

        set result [r gcra mykey 5 1 60]
        set full_burst_after [lindex $result 4]

        # full_burst_after should be positive (time until full burst available)
        # Since we've taken from the burst twice the reset time was incremented
        # by the rate twice
        assert {$full_burst_after >= 179}

        r del mykey
        r gcra mykey 5 1 60
        set result [r gcra mykey 5 1 60 TOKENS 4]
        set full_burst_after [lindex $result 4]
        assert {$full_burst_after >= 299}
    }

    test {GCRA - different keys are independent} {
        r del key1
        r del key2

        # Exhaust key1
        for {set i 0} {$i < 3} {incr i} {
            r gcra key1 2 1 60
        }
        set result1 [r gcra key1 2 1 60]
        assert_equal 1 [lindex $result1 0] "key1 should be limited"

        # key2 should still be available
        set result2 [r gcra key2 2 1 60]
        assert_equal 0 [lindex $result2 0] "key2 should be allowed"
    }

    test {GCRA - max_burst of 0 allows only sustained rate} {
        r del mykey
        # max_burst=0, tokens_per_period=1, period=1
        # Only 1 request allowed initially (0+1)
        set result [r gcra mykey 0 1 1]
        assert_equal 0 [lindex $result 0] "First request allowed"
        assert_equal 1 [lindex $result 1] "max_req_num should be 1"

        # Second request should be limited
        set result [r gcra mykey 0 1 1]
        assert_equal 1 [lindex $result 0] "Second request limited"
    }

    test {GCRA - wrong type error} {
        r del mykey
        r lpush mykey "value"
        catch {r gcra mykey 5 1 60} err
        assert_match "*WRONGTYPE*" $err

        r del mykey
        r sadd mykey "value"
        catch {r gcra mykey 5 1 60} err
        assert_match "*WRONGTYPE*" $err
    }

    test {GCRA - overflow} {
        r del mykey
        catch {r gcra mykey 1 1 86400 TOKENS 200000000} err
        assert_match "*would cause an overflow*" $err

        r del mykey
        catch {r gcra mykey 200000000 1 86400} err
        assert_match "*would cause an overflow*" $err

        r del mykey
        catch {r gcra mykey 1 1 2147483647 TOKENS 2147483647} err
        assert_match "*would cause an overflow*" $err
    }
}

start_server {tags {"gcra" "external:skip"}} {
    test {GCRA - RDB save and reload preserves value} {
        r del mykey
        r gcra mykey 5 1 60
        r gcra mykey 5 1 60

        set dump_before [r dump mykey]

        r debug reload

        assert_equal [r type mykey] "gcra"
        set dump_after [r dump mykey]
        assert_equal $dump_before $dump_after
    } {} {needs:debug}

    test {GCRA - RDB save and reload preserves TTL} {
        r del mykey
        r gcra mykey 5 1 60
        set ttl_before [r pexpiretime mykey]
        assert_morethan $ttl_before 0

        r debug reload

        set ttl_after [r pexpiretime mykey]
        assert_morethan $ttl_after 0
        assert_equal $ttl_after $ttl_before
    } {} {needs:debug}

    test {GCRA - DUMP and RESTORE roundtrip} {
        r del mykey mykey2
        r gcra mykey 5 1 60
        r gcra mykey 5 1 60

        set dump [r dump mykey]
        set ttl [r pttl mykey]
        r restore mykey2 $ttl $dump

        assert_equal [r type mykey2] "gcra"

        set result_orig [r gcra mykey 5 1 60]
        set result_restored [r gcra mykey2 5 1 60]
        assert_equal [lindex $result_orig 2] [lindex $result_restored 2]
    }

    test {GCRA - AOF rewrite preserves value} {
        r del mykey
        r config set appendonly yes
        waitForBgrewriteaof r

        r gcra mykey 5 1 60
        r gcra mykey 5 1 60

        set dump_before [r dump mykey]

        r BGREWRITEAOF
        waitForBgrewriteaof r
        r debug reload

        assert_equal [r type mykey] "gcra"
        set dump_after [r dump mykey]
        assert_equal $dump_before $dump_after
    } {} {external:skip needs:debug}

    test {GCRA - AOF rewrite preserves TTL} {
        r del mykey
        r config set appendonly yes
        waitForBgrewriteaof r

        r gcra mykey 5 1 60

        r BGREWRITEAOF
        waitForBgrewriteaof r

        set ttl_before [r pttl mykey]
        assert {$ttl_before > 0}

        r debug reload

        set ttl_after [r pttl mykey]
        assert {$ttl_after > 0}
        assert {$ttl_after <= $ttl_before}
    } {} {external:skip needs:debug}

    test {GCRA - DEBUG DIGEST consistent after RDB reload} {
        r del mykey
        r gcra mykey 5 1 60
        r gcra mykey 5 1 60

        set digest_before [r debug digest]

        r debug reload

        set digest_after [r debug digest]
        assert_equal $digest_before $digest_after
    } {} {needs:debug}
}

start_server {tags {"gcra repl" "external:skip"}} {
    set replica [srv 0 client]
    set replica_host [srv 0 host]
    set replica_port [srv 0 port]
    set replica_log [srv 0 stdout]

    start_server {tags {}} {
        set master [srv 0 client]
        set master_host [srv 0 host]
        set master_port [srv 0 port]

        test {GCRA - Replication works} {
            $master flushdb
            $replica flushdb

            $replica replicaof $master_host $master_port
            wait_for_condition 100 100 {
                [s -1 master_link_status] eq "up"
            } else {
                fail "Master <-> Replica didn't finish sync"
            }

            set cmdinfo [$replica info commandstats]
            assert_equal [lsearch -glob $cmdinfo "cmdstat_gcrasetvalue:*"] -1

            $master del mykey
            $master gcra mykey 2 1 1000 TOKENS 2
            wait_for_ofs_sync $master $replica

            set cmdinfo [$replica info commandstats]
            assert_morethan_equal [lsearch -glob $cmdinfo "cmdstat_gcrasetvalue:*"] 0
        } {} {external:skip}
    }
}
