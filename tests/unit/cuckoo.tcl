# DUMP from RedisBloom: CF.RESERVE cf 4 BUCKETSIZE 2 MAXITERATIONS 20 EXPANSION 2,
# followed by CF.ADD for binary items {i,0,255}, i=0..29, then CF.DEL for i=0.
proc legacy_cuckoo_payload {} {
    return [binary format H* 07813016e5a28982140402040202021d020102020214020202020504221a005e02040508f09034dbed62d03302080510da390a446f00c787000004783e006eac0210052000000000000000007d000000d200000000000000a000af00b800500000000000000d00e60077b1b6c1a52a]
}

start_server {tags {"cuckoo"} overrides {save ""}} {
    test {Legacy Cuckoo DUMP loads as a native object without RedisBloom} {
        assert_equal OK [r restore cf 0 [legacy_cuckoo_payload]]
        assert_equal cuckoo [r type cf]
        assert_equal cuckoo [r object encoding cf]
        foreach module [r module list] {
            assert {[dict get $module name] ne "bf"}
        }
    }

    test {Native Cuckoo supports COPY memory accounting and SCAN TYPE} {
        assert {[r memory usage cf] > 100}
        assert_equal 1 [r copy cf copied]
        assert_equal [r dump cf] [r dump copied]
        assert_equal [r debug digest-value cf] [r debug digest-value copied]
        assert_equal {cf copied} [lsort [lindex [r scan 0 type cuckoo] 1]]
        assert_equal 1 [r unlink copied]
    } {} {needs:debug}

    test {Other type commands reject native Cuckoo objects} {
        assert_error WRONGTYPE* {r get cf}
        assert_error WRONGTYPE* {r sadd cf x}
        assert_error WRONGTYPE* {r bf.add cf x}
    }

    test {Native Cuckoo DUMP RESTORE preserves contents} {
        set payload [r dump cf]
        assert_equal OK [r restore copied 0 $payload]
        assert_equal $payload [r dump copied]
        assert_equal 1 [r del copied]
    }

    test {Cuckoo survives native RDB reload and redis-check-rdb} {
        set payload [r dump cf]
        r pexpire cf 60000
        r debug reload
        assert_equal $payload [r dump cf]
        assert {[r pttl cf] > 0}
        set path [file join [lindex [r config get dir] 1] dump.rdb]
        assert_match {*RDB looks OK*} [exec src/redis-check-rdb $path]
        r persist cf
    } {1} {needs:debug}

    test {Cuckoo rejects corrupt metadata and truncated native and module payloads} {
        r debug set-skip-checksum-validation 1
        set payload [r dump cf]
        # Native one-byte fields: filter count, base buckets, bucket size,
        # max iterations, expansion, and first subfilter buckets.
        foreach {offset value} {1 0 2 0 2 3 5 0 6 0 7 0 8 0 8 3 9 3 1 192} {
            set bad [string replace $payload $offset $offset [binary format c $value]]
            assert_error {*Bad data format*} {r restore bad 0 $bad}
            assert_equal 0 [r exists bad]
        }
        set legacy [legacy_cuckoo_payload]
        # Reject an unsupported module version, an invalid unsigned opcode,
        # and an invalid final module EOF opcode.
        foreach offset [list 9 10 [expr {[string length $legacy]-11}]] {
            set bad [string replace $legacy $offset $offset [binary format c 5]]
            assert_error {*Bad data format*} {r restore bad 0 $bad}
            assert_equal 0 [r exists bad]
        }
        r debug set-skip-checksum-validation 0
        foreach payload [list $payload [legacy_cuckoo_payload]] {
            for {set cut 1} {$cut < [string length $payload]-10} {incr cut} {
                set bad "[string range $payload 0 [expr {$cut-1}]][string range $payload end-9 end]"
                assert_error {*DUMP payload version or checksum are wrong*} {r restore bad 0 $bad}
                assert_equal 0 [r exists bad]
            }
        }
        r debug set-skip-checksum-validation 0
        assert_equal PONG [r ping]
    } {} {needs:debug}

    foreach preamble {no yes} {
        test "Cuckoo survives AOF rewrite with RDB preamble $preamble" {
            r config set aof-use-rdb-preamble $preamble
            r config set appendonly yes
            waitForBgrewriteaof r
            set payload [r dump cf]
            r pexpire cf 60000
            r bgrewriteaof
            waitForBgrewriteaof r
            r debug loadaof
            assert_equal $payload [r dump cf]
            assert {[r pttl cf] > 0}
            r persist cf
            r config set appendonly no
        } {OK} {needs:debug}
    }
}

start_server {tags {"cuckoo external:skip"} overrides {save ""}} {
    test {Cuckoo persistence defaults support CONFIG SET and bounds} {
        foreach {name default minimum maximum} {
            cf-bucket-size 2 1 255
            cf-max-iterations 20 1 65535
            cf-expansion-factor 1 0 32768
        } {
            assert_equal $default [lindex [r config get $name] 1]
            assert_equal OK [r config set $name $minimum]
            assert_equal OK [r config set $name $maximum]
            assert_error {*} {r config set $name [expr {$minimum-1}]}
            assert_error {*} {r config set $name [expr {$maximum+1}]}
            assert_equal $maximum [lindex [r config get $name] 1]
            r config set $name $default
        }
    }
    test {Cuckoo persistence defaults survive CONFIG REWRITE and restart} {
        r config set cf-bucket-size 4 cf-max-iterations 31 cf-expansion-factor 2
        r config rewrite
        restart_server 0 true false
        assert_equal 4 [lindex [r config get cf-bucket-size] 1]
        assert_equal 31 [lindex [r config get cf-max-iterations] 1]
        assert_equal 2 [lindex [r config get cf-expansion-factor] 1]
    }
}
