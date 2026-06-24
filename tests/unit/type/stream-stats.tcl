################################################################################
# Test that streams are tracked in the `# Keysizes` INFO section, under
# distrib_streams_items: a per-database base-2 logarithmic histogram of stream
# entry counts. Streams are treated like any other key type -- the sample is
# maintained from the stream commands (append/trim/delete) and from the generic
# key lifecycle hooks (birth on dbAdd, death on delete/expire), so collection is
# always-on and exact (no directive to enable it).
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

start_server {tags {"stream cluster:skip"}} {
    test {distrib_streams_items places streams in the expected bins} {
        r select 9
        r flushall
        r xadd a 1-1 f v                                   ;# 1 -> "1"
        r xadd b 1-1 f v; r xadd b 2-1 f v                 ;# 2 -> "2"
        for {set i 1} {$i <= 5} {incr i} { r xadd c $i-1 f v } ;# 5 -> "4"
        assert_equal "1=1,2=1,4=1" [items_hist r]
    }

    test {Sample follows the stream across power-of-two boundaries} {
        r select 9
        r flushall
        r xadd k 1-1 f v
        assert_equal "1=1" [items_hist r]
        r xadd k 2-1 f v
        assert_equal "2=1" [items_hist r]
        r xadd k 3-1 f v; r xadd k 4-1 f v
        assert_equal "4=1" [items_hist r]
    }

    test {XADD with MAXLEN trims within the command and updates the bin} {
        r select 9
        r flushall
        for {set i 1} {$i <= 8} {incr i} { r xadd k MAXLEN 4 $i-1 f v }
        assert_equal 4 [r xlen k]
        assert_equal "4=1" [items_hist r]
    }

    test {XTRIM and XDEL move the sample down} {
        r select 9
        r flushall
        for {set i 1} {$i <= 8} {incr i} { r xadd k $i-1 f v }
        assert_equal "8=1" [items_hist r]
        r xdel k 8-1 7-1 6-1 5-1
        assert_equal "4=1" [items_hist r]
        r xtrim k maxlen 1
        assert_equal "1=1" [items_hist r]
    }

    test {XDELEX updates the items histogram} {
        r select 9
        r flushall
        for {set i 1} {$i <= 4} {incr i} { r xadd k $i-1 f v }
        assert_equal "4=1" [items_hist r]
        r xdelex k IDS 2 4-1 3-1
        assert_equal 2 [r xlen k]
        assert_equal "2=1" [items_hist r]
    }

    test {XACKDEL updates the items histogram} {
        r select 9
        r flushall
        for {set i 1} {$i <= 4} {incr i} { r xadd k $i-1 f v }
        r xgroup create k g 0
        r xreadgroup group g c count 4 streams k >
        assert_equal "4=1" [items_hist r]
        r xackdel k g IDS 2 1-1 2-1
        assert_equal 2 [r xlen k]
        assert_equal "2=1" [items_hist r]
    }

    test {An emptied stream is still counted in bin 0} {
        r select 9
        r flushall
        r xadd k 1-1 f v
        r xdel k 1-1
        assert_equal 0 [r xlen k]
        assert_equal "0=1" [items_hist r]
    }

    test {XGROUP CREATE MKSTREAM counts the new empty stream in bin 0} {
        r select 9
        r flushall
        r xgroup create k g 0 mkstream
        assert_equal 0 [r xlen k]
        assert_equal "0=1" [items_hist r]
    }

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

    tags {"needs:debug"} {
        test {Histogram is reconstructed from RDB on DEBUG RELOAD} {
            r select 9
            r flushall
            r xadd a 1-1 f v
            for {set i 1} {$i <= 5} {incr i} { r xadd c $i-1 f v }
            set before [items_hist r]
            r debug reload
            assert_equal $before [items_hist r]
            assert_equal "1=1,4=1" [items_hist r]
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

    test {FLUSHALL resets the histogram} {
        r select 9
        r flushall
        r xadd a 1-1 f v
        assert_equal "1=1" [items_hist r]
        r flushall
        assert_equal "" [items_hist r]
    }
}
