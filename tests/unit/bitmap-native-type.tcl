start_server {tags {"bitmap" "bitmap-native" "needs:debug" "cluster:skip"}} {
    test {native bitmap helper exposes type encoding and exact raw bytes} {
        set raw [binary format H* 80400100080000]

        r set bitmap:raw $raw
        assert_equal [r debug bitmap-force-roaring bitmap:raw] OK
        assert_equal [r type bitmap:raw] bitmap
        assert_equal [r object encoding bitmap:raw] bitmap-roaring
        assert_equal [r debug bitmap-raw bitmap:raw] $raw
        assert_error {WRONGTYPE*} {r get bitmap:raw}
    }

    test {native bitmap scan type and copy preserve bitmap objects} {
        set raw [binary format H* 010204000000]

        r set bitmap:copy-source $raw
        r set bitmap:string-peer value
        r debug bitmap-force-roaring bitmap:copy-source

        # SCAN gives no guarantee that one page covers the keyspace, so walk
        # the cursor to completion before asserting membership.
        set keys {}
        set cursor 0
        while 1 {
            set scan_reply [r scan $cursor type bitmap]
            set cursor [lindex $scan_reply 0]
            foreach key [lindex $scan_reply 1] { lappend keys $key }
            if {$cursor == 0} break
        }
        assert {[lsearch -exact $keys bitmap:copy-source] >= 0}
        assert {[lsearch -exact $keys bitmap:string-peer] == -1}

        assert_equal [r copy bitmap:copy-source bitmap:copy-target] 1
        assert_equal [r type bitmap:copy-target] bitmap
        assert_equal [r object encoding bitmap:copy-target] bitmap-roaring
        assert_equal [r debug bitmap-raw bitmap:copy-target] $raw
    }

    test {native bitmap dump restore and debug reload preserve bitmap objects} {
        set raw [binary format H* f0000000000000010000]

        r set bitmap:persist $raw
        r debug bitmap-force-roaring bitmap:persist

        set payload [r dump bitmap:persist]
        r restore bitmap:restored 0 $payload
        assert_equal [r type bitmap:restored] bitmap
        assert_equal [r object encoding bitmap:restored] bitmap-roaring
        assert_equal [r debug bitmap-raw bitmap:restored] $raw

        r debug reload
        assert_equal [r type bitmap:persist] bitmap
        assert_equal [r object encoding bitmap:persist] bitmap-roaring
        assert_equal [r debug bitmap-raw bitmap:persist] $raw
        assert_equal [r type bitmap:restored] bitmap
        assert_equal [r debug bitmap-raw bitmap:restored] $raw
    }

    test {native bitmap RDB payload endianness conversion round-trips} {
        # Build one bitmap holding all three container kinds so the converter
        # walks every payload section. Each 65536-bit chunk is 8192 bytes:
        # chunk 0: 4800 consecutive set bits -> run container
        # chunk 1: alternating bits, cardinality 8000 -> bitset container
        # chunk 2: 64 isolated bits -> array container
        # chunk 3: another run container, lifting the container count to the
        #          CRoaring offset-header threshold so the offsets section is
        #          present alongside the run bitmap.
        set raw [string repeat [binary format H* ff] 600]
        append raw [string repeat [binary format H* 00] 7592]
        append raw [string repeat [binary format H* aa] 2000]
        append raw [string repeat [binary format H* 00] 6192]
        for {set i 0} {$i < 64} {incr i} {
            append raw [binary format H* 80][string repeat [binary format H* 00] 15]
        }
        append raw [string repeat [binary format H* 00] 7168]
        append raw [string repeat [binary format H* ff] 600]

        r set bitmap:endian $raw
        r debug bitmap-force-roaring bitmap:endian
        assert_equal OK [r debug bitmap-endian-check bitmap:endian]
        assert_equal [r debug bitmap-raw bitmap:endian] $raw

        # An array-only bitmap spanning two containers serializes with the
        # no-run cookie, covering the other header layout.
        set sparse [binary format H* 80]
        append sparse [string repeat [binary format H* 00] 8191]
        append sparse [binary format H* 80]
        r set bitmap:endian-sparse $sparse
        r debug bitmap-force-roaring bitmap:endian-sparse
        assert_equal OK [r debug bitmap-endian-check bitmap:endian-sparse]
        assert_equal [r debug bitmap-raw bitmap:endian-sparse] $sparse
    }

    test {native bitmap unlink uses lazyfree for many roaring containers} {
        r config resetstat
        for {set i 0} {$i < 80} {incr i} {
            r setbit bitmap:lazy [expr {$i * 65536}] 1
        }
        r debug bitmap-force-roaring bitmap:lazy
        assert_equal [r type bitmap:lazy] bitmap

        assert_equal [r unlink bitmap:lazy] 1
        wait_for_condition 50 100 {
            [s lazyfree_pending_objects] == 0
        } else {
            fail "lazyfree isn't done"
        }
        assert_equal [s lazyfreed_objects] 1
    } {} {needs:config-resetstat}
}

start_server {tags {"bitmap" "bitmap-native" "needs:debug" "external:skip" "cluster:skip" "logreqres:skip"} overrides {save {} aof-use-rdb-preamble no}} {
    test {native bitmap survives AOF rewrite as bitmap} {
        r config set appendonly yes
        r config set auto-aof-rewrite-percentage 0
        waitForBgrewriteaof r

        set raw [binary format H* 80000000000000000001]

        r set bitmap:aof $raw
        r debug bitmap-force-roaring bitmap:aof
        set digest_before [debug_digest]

        r bgrewriteaof
        waitForBgrewriteaof r
        r debug loadaof

        assert_equal [debug_digest] $digest_before
        assert_equal [r type bitmap:aof] bitmap
        assert_equal [r object encoding bitmap:aof] bitmap-roaring
        assert_equal [r debug bitmap-raw bitmap:aof] $raw
    }
}
