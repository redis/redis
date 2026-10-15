# Native Bloom fixture: ten binary items {i, 0, 255}, i=0..9, inserted into
# RedisBloom with capacity 2, error 0.01, expansion 2, FORCE64 | NOROUND.
# Three filters, encoded using the native RDB format and a valid DUMP checksum.
proc native_bloom_payload {} {
    return [binary format H* 230a030502027b14ae47e17a743f08e9862fb2350e264040400008008808008800e09f02047b14ae47e17a643f094af7d49edef028404040000855c8c8405527260604087b14ae47e17a543f0aaa677a8b87d32b404080001054128444444c24a005010d08540424020411006c4d19c6a8d7d3a8]
}

start_server {tags {"bloom"}} {
    test {Native Bloom reports its type and encoding without a module} {
        assert_equal OK [r restore bloom 0 [native_bloom_payload]]
        assert_equal bloom [r type bloom]
        assert_equal bloom [r object encoding bloom]
        assert_equal {} [lindex [r command info bf.add] 0]
    }

    test {Native Bloom supports memory accounting, COPY, and SCAN TYPE} {
        assert {[r memory usage bloom] > 100}
        assert_equal 1 [r copy bloom copied]
        assert_equal [r dump bloom] [r dump copied]
        assert_equal [r debug digest-value bloom] [r debug digest-value copied]
        assert_equal {bloom copied} [lsort [lindex [r scan 0 type bloom] 1]]
        r del bloom
        assert_equal bloom [r type copied]
        r rename copied bloom
    }

    test {Other data type commands reject native Bloom objects} {
        assert_error WRONGTYPE* {r get bloom}
        assert_error WRONGTYPE* {r sadd bloom x}
        assert_error WRONGTYPE* {r pfadd bloom x}
    }

    test {Bloom DUMP RESTORE round trip preserves filter data} {
        set payload [r dump bloom]
        r restore restored 0 $payload
        assert_equal $payload [r dump restored]
        r unlink restored
        assert_equal 0 [r exists restored]
    }

    test {Native Bloom survives RDB reload with expiry and digest intact} {
        r pexpire bloom 60000
        set digest [r debug digest-value bloom]
        r debug reload
        set dump_path [file join [lindex [r config get dir] 1] dump.rdb]
        assert_match {*RDB looks OK*} [exec src/redis-check-rdb $dump_path]
        assert_equal bloom [r type bloom]
        assert_equal $digest [r debug digest-value bloom]
        assert {[r pttl bloom] > 0}
        r persist bloom
    } {1} {needs:debug}

    test {Native Bloom rejects malformed RDB fields and truncated payloads} {
        r debug set-skip-checksum-validation 1
        set payload [native_bloom_payload]
        # Count mismatch, no filters, invalid options/growth/hash count/n2,
        # a per-filter count exceeding capacity, and an encoded integer tag.
        foreach {offset value} {1 0 2 0 3 255 4 0 14 0 25 64 35 3 2 192} {
            set bad [string replace $payload $offset $offset [binary format c $value]]
            assert_error {*Bad data format*} {r restore bad 0 $bad}
            assert_equal 0 [r exists bad]
        }
        set bad [string replace $payload 6 13 [binary format H* 000000000000f87f]]
        assert_error {*Bad data format*} {r restore bad 0 $bad}
        for {set cut 1} {$cut < [string length $payload]-10} {incr cut} {
            set bad "[string range $payload 0 [expr {$cut-1}]][string range $payload end-9 end]"
            assert_error {*Bad data format*} {r restore bad 0 $bad}
        }
        r debug set-skip-checksum-validation 0
        assert_equal PONG [r ping]
    } {} {needs:debug}

    foreach preamble {no yes} {
        test "Native Bloom survives AOF rewrite with RDB preamble $preamble" {
            r config set aof-use-rdb-preamble $preamble
            r config set appendonly yes
            waitForBgrewriteaof r
            set payload [r dump bloom]
            r pexpire bloom 60000
            r bgrewriteaof
            waitForBgrewriteaof r
            assert_equal ok [s aof_last_bgrewrite_status]
            r debug loadaof
            assert_equal $payload [r dump bloom]
            assert {[r pttl bloom] > 0}
            r persist bloom
            r config set appendonly no
        } {OK} {needs:debug}
    }

    test {Native Bloom expiration and deletion free the key} {
        r pexpire bloom 1
        wait_for_condition 50 10 {![r exists bloom]} else {fail "Bloom key did not expire"}
        r restore bloom 0 [native_bloom_payload]
        assert_equal 1 [r del bloom]
    }
}

if {$::debug_defrag} {
    start_server {tags {"bloom defrag needs:debug external:skip cluster:skip"} overrides {save "" key-memory-histograms yes}} {
        foreach scan_fields {1000 1} {
            test "Forced Bloom defrag preserves contents with scan limit $scan_fields" {
                r restore bloom 0 [native_bloom_payload] replace
                set payload [r dump bloom]
                r config resetstat
                r config set active-defrag-max-scan-fields $scan_fields
                r config set hz 100
                r config set active-defrag-ignore-bytes 1
                r config set active-defrag-threshold-lower 1 active-defrag-threshold-upper 1
                r config set active-defrag-cycle-min 65 active-defrag-cycle-max 75
                r config set activedefrag yes
                wait_for_condition 500 10 {[s active_defrag_key_hits] > 0} else {
                    fail "Bloom key was not relocated"
                }
                r config set activedefrag no
                wait_for_condition 500 10 {[s active_defrag_running] == 0} else {
                    fail "Bloom defrag did not stop"
                }
                assert_equal $payload [r dump bloom]
                assert {[r memory usage bloom] > 100}
            }
        }
    }
}
