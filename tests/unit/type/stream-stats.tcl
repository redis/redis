################################################################################
# Verify that streams stay correctly tracked in the `# Keysizes` INFO section
# (db<N>_distrib_streams_items) across the generic key-lifecycle operations --
# delete variants, overwrite, rename, copy, restore, expiration, and the
# per-database operations (move, swapdb, flushdb).
#
# The stream-specific length accounting (XADD/XTRIM/XDEL/XDELEX/XACKDEL bin
# placement, MKSTREAM, RDB reload) is covered exhaustively in info-keysizes.tcl.
# Here we only cover the type-agnostic keyspace paths, exercised against a stream
# and read straight from the INFO field, to confirm streams flow through them.
#
# Note: the test harness selects DB 9 by default, so single-DB tests assert on
# the db9_ field; multi-DB tests select their DBs explicitly.
################################################################################

# Return the value of the stream items histogram for a db (e.g. "2=1,16=1"), or
# "" when no stream key exists in that db. The field lives in `# Keysizes`.
proc items_hist {r {dbnum 9}} {
    foreach line [split [$r info keysizes] "\n"] {
        set line [string trim $line "\r"]
        if {[regexp "^db${dbnum}_distrib_streams_items:(.*)$" $line -> val]} { return $val }
    }
    return ""
}

start_server {tags {stream cluster:skip external:skip}} {
    # Every key-delete command must drop the deleted stream's sample (and only
    # it). We run each command for real rather than assume they share a path.
    foreach delcmd {del unlink delex} {
        test "$delcmd removes only the deleted stream's sample" {
            r select 9
            r flushall
            r xadd a 1-1 f v
            r xadd b 1-1 f v
            assert_equal "1=2" [items_hist r]
            r $delcmd a
            assert_equal "1=1" [items_hist r]
            r $delcmd b
            assert_equal 0 [r exists b]
            assert_equal "" [items_hist r]
        }
    }

    test {Overwriting a stream key with a string removes its sample} {
        r select 9
        r flushall
        r xadd k 1-1 f v
        assert_equal "1=1" [items_hist r]
        r set k "now a string"
        assert_equal "" [items_hist r]
    }

    # Every rename command moves the sample to the new key name, keeping the
    # stream counted exactly once.
    foreach rencmd {rename renamenx} {
        test "$rencmd keeps the stream counted once" {
            r select 9
            r flushall
            r xadd a 1-1 f v
            r $rencmd a b
            assert_equal 0 [r exists a]
            assert_equal "1=1" [items_hist r]
        }
    }

    test {COPY counts the duplicated stream} {
        r select 9
        r flushall
        r xadd a 1-1 f v
        r copy a b
        assert_equal "1=2" [items_hist r]
    }

    test {RESTORE counts the recreated stream} {
        r select 9
        r flushall
        for {set i 1} {$i <= 4} {incr i} { r xadd a $i-1 f v }
        set dump [r dump a]
        r del a
        assert_equal "" [items_hist r]
        r restore a 0 $dump
        assert_equal "4=1" [items_hist r]
    }

    test {Key expiration removes the stream sample} {
        r select 9
        r flushall
        r xadd k 1-1 f v
        assert_equal "1=1" [items_hist r]
        r pexpire k 10
        wait_for_condition 50 20 {
            [items_hist r] eq ""
        } else {
            fail "stream sample not removed after expiration"
        }
    }

    test {Per-database lines are independent} {
        r flushall
        r select 0
        r xadd a 1-1 f v
        r select 5
        r xadd a 1-1 f v; r xadd a 2-1 f v
        assert_equal "1=1" [items_hist r 0]
        assert_equal "2=1" [items_hist r 5]
        r select 9
    }

    test {MOVE relocates the stream's sample to the destination db} {
        r select 9
        r flushall
        r xadd k 1-1 f v; r xadd k 2-1 f v                  ;# 2 entries -> "2"
        assert_equal "2=1" [items_hist r 9]
        assert_equal "" [items_hist r 10]
        r move k 10
        assert_equal "" [items_hist r 9]
        assert_equal "2=1" [items_hist r 10]
        r flushall
        r select 9
    }

    test {SWAPDB swaps the per-db histograms along with the keyspaces} {
        r select 9
        r flushall
        for {set i 1} {$i <= 4} {incr i} { r xadd a $i-1 f v }   ;# db9: 4 -> "4"
        r select 10
        for {set i 1} {$i <= 8} {incr i} { r xadd b $i-1 f v }   ;# db10: 8 -> "8"
        assert_equal "4=1" [items_hist r 9]
        assert_equal "8=1" [items_hist r 10]
        r swapdb 9 10
        assert_equal "8=1" [items_hist r 9]
        assert_equal "4=1" [items_hist r 10]
        r flushall
        r select 9
    }

    test {FLUSHDB clears one db's histogram and leaves others intact} {
        r select 9
        r flushall
        r xadd a 1-1 f v; r xadd a 2-1 f v                      ;# db9: 2 -> "2"
        r select 10
        for {set i 1} {$i <= 4} {incr i} { r xadd b $i-1 f v }  ;# db10: 4 -> "4"
        assert_equal "2=1" [items_hist r 9]
        assert_equal "4=1" [items_hist r 10]
        r select 9
        r flushdb
        assert_equal "" [items_hist r 9]
        assert_equal "4=1" [items_hist r 10]
        r flushall
        r select 9
    }
}
