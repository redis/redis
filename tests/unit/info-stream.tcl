################################################################################
# Test the "INFO stream" section.
#
# The section reports per-database, base-2 logarithmic histograms of stream
# properties, identical in form to the "INFO keysizes" section. The first
# metric is distrib_cgroups_pel: one sample per consumer group, valued by the
# group's pending-entry-list (PEL) size. Collection is gated on the stream-stats
# directive and reconstructed exactly from RDB / replication.
################################################################################

# Map a PEL value to its histogram bin label, matching the C binning
# (largest power of two <= value; 0 -> "0"; >=1024 rendered with K/M/... suffix).
proc pel_label {v} {
    if {$v == 0} { return 0 }
    set power 1
    while { ($power * 2) <= $v } { set power [expr {$power * 2}] }
    if {$power >= 1048576} { return "[expr {$power / 1048576}]M" }
    if {$power >= 1024}    { return "[expr {$power / 1024}]K" }
    return $power
}

# Strip the "INFO stream" output to a canonical comparable form: drop the
# "# Stream" header and all whitespace.
proc get_info_stream_stripped {server} {
    return [string map {
        "# Stream" ""
        " " "" "\n" "" "\r" ""
    } [$server info stream]]
}

# Reconstruct the expected distrib_cgroups_pel histogram for 'dbid' directly
# from the keyspace (the INFO-independent cross-check): for every stream, ask
# XINFO GROUPS for each group's pending count and bin it. Returns the same
# canonical (stripped) form as get_info_stream_stripped, so the two can be
# compared for equality.
proc eval_pel_histogram {server dbid} {
    $server select $dbid
    array set bin_counts {}
    foreach key [$server keys *] {
        if {[$server type $key] ne "stream"} continue
        foreach g [$server xinfo groups $key] {
            array set gi $g
            incr bin_counts([pel_label $gi(pending)])
            unset gi
        }
    }
    if {![array size bin_counts]} { return "" }

    # Sort bins by their numeric power (decode K/M suffixes back to a number).
    set pairs {}
    foreach label [array names bin_counts] {
        set power $label
        if {[string match "*K" $label]} { set power [expr {[string trimright $label K] * 1024}] }
        if {[string match "*M" $label]} { set power [expr {[string trimright $label M] * 1048576}] }
        lappend pairs [list $power "$label=$bin_counts($label)"]
    }
    set out {}
    foreach p [lsort -integer -index 0 $pairs] { lappend out [lindex $p 1] }
    return "db${dbid}_distrib_cgroups_pel:[join $out ,]"
}

# Resolve the expected string: the sentinel "__EVAL__ <dbid>" reconstructs from
# the keyspace; otherwise the literal (placeholder "PEL" -> the field name).
proc pel_expand {server exp} {
    if {[regexp {^__EVAL__\s+(\d+)$} $exp -> dbid]} {
        return [eval_pel_histogram $server $dbid]
    }
    return [string map {"PEL" "distrib_cgroups_pel" " " "" "\n" "" "\r" ""} $exp]
}

# Run 'cmd', then assert the INFO stream section equals 'exp'. In replicaMode the
# assertion is repeated on the replica after it catches up.
proc verify_pel {cmd exp {waitCond 0}} {
    global replicaMode
    uplevel 1 $cmd

    if {$replicaMode eq 1} {
        set server [srv -1 client]
        set replica [srv 0 client]
    } else {
        set server [srv 0 client]
    }

    set retries [expr {$waitCond ? 50 : 1}]

    wait_for_condition 50 $retries {
        [pel_expand $server $exp] eq [get_info_stream_stripped $server]
    } else {
        fail "Expected: `[pel_expand $server $exp]` Actual: `[get_info_stream_stripped $server]`. After: $cmd"
    }

    if {$replicaMode eq 1} {
        wait_for_condition 50 50 {
            [pel_expand $server $exp] eq [get_info_stream_stripped $replica]
        } else {
            fail "Replica mismatch. Expected: `[pel_expand $server $exp]` Actual: `[get_info_stream_stripped $replica]`. After: $cmd"
        }
    }
}

# Seed a stream with 'n' entries 1-1..n-1.
proc seed_stream {server key n} {
    for {set i 1} {$i <= $n} {incr i} { $server xadd $key $i-1 f v }
}

proc test_all_stream_stats { {replMode 0} } {
    global replicaMode
    set replicaMode $replMode
    if {$replicaMode eq 1} {
        set server [srv -1 client]
        set suffix "(replica)"
    } else {
        set server [srv 0 client]
        set suffix ""
    }

    test "STREAM-STATS - PEL bin boundaries 1,2,4,8,... $suffix" {
        # Read exactly n entries into a fresh group -> PEL = n, which must land
        # in the bin for the largest power of two <= n.
        foreach n {1 2 3 4 7 8 15 16 300 512} {
            verify_pel {$server FLUSHALL} {}
            seed_stream $server st $n
            $server xgroup create st g 0
            verify_pel {$server xreadgroup group g c count $n streams st >} "db0_PEL:[pel_label $n]=1"
        }
    }

    test "STREAM-STATS - empty group counts in bin 0 $suffix" {
        verify_pel {$server FLUSHALL} {}
        verify_pel {$server xgroup create st g0 0 mkstream} {db0_PEL:0=1}
    }

    test "STREAM-STATS - XREADGROUP grows, XACK shrinks $suffix" {
        verify_pel {$server FLUSHALL} {}
        seed_stream $server st 4
        $server xgroup create st g 0
        verify_pel {} {db0_PEL:0=1}
        verify_pel {$server xreadgroup group g c count 4 streams st >} {db0_PEL:4=1}
        verify_pel {$server xack st g 1-1} {db0_PEL:2=1}
        verify_pel {$server xack st g 2-1 3-1} {db0_PEL:1=1}
        verify_pel {$server xack st g 4-1} {db0_PEL:0=1}
    }

    test "STREAM-STATS - XACKDEL shrinks PEL $suffix" {
        verify_pel {$server FLUSHALL} {}
        seed_stream $server st 4
        $server xgroup create st g 0
        $server xreadgroup group g c count 4 streams st >
        verify_pel {} {db0_PEL:4=1}
        verify_pel {$server xackdel st g ids 2 1-1 2-1} {db0_PEL:2=1}
    }

    test "STREAM-STATS - XNACK FORCE grows PEL $suffix" {
        verify_pel {$server FLUSHALL} {}
        seed_stream $server st 4
        verify_pel {$server xgroup create st g 0} {db0_PEL:0=1}
        # FORCE creates unowned PEL entries for existing stream IDs.
        verify_pel {$server xnack st g SILENT IDS 3 1-1 2-1 3-1 FORCE} {db0_PEL:2=1}
    }

    test "STREAM-STATS - XCLAIM FORCE grows, claim of deleted shrinks $suffix" {
        verify_pel {$server FLUSHALL} {}
        seed_stream $server st 4
        $server xgroup create st g 0
        # XCLAIM FORCE creates PEL entries for the listed (existing) IDs.
        verify_pel {$server xclaim st g c 0 1-1 2-1 3-1 FORCE} {db0_PEL:2=1}
        # Delete an entry from the stream, then XCLAIM purges its dangling PEL ref.
        $server xdel st 1-1
        verify_pel {$server xclaim st g c2 0 1-1 2-1 3-1 FORCE} {db0_PEL:2=1}
    }

    test "STREAM-STATS - XAUTOCLAIM purges deleted PEL entries $suffix" {
        verify_pel {$server FLUSHALL} {}
        seed_stream $server st 4
        $server xgroup create st g 0
        $server xreadgroup group g c count 4 streams st >
        verify_pel {} {db0_PEL:4=1}
        # Delete two entries; their dangling PEL refs are purged on autoclaim.
        $server xdel st 1-1 2-1
        verify_pel {$server xautoclaim st g c2 0 0} {db0_PEL:2=1}
    }

    test "STREAM-STATS - XGROUP DELCONSUMER removes its PEL entries $suffix" {
        verify_pel {$server FLUSHALL} {}
        seed_stream $server st 4
        $server xgroup create st g 0
        $server xreadgroup group g c1 count 3 streams st >
        $server xreadgroup group g c2 count 1 streams st >
        verify_pel {} {db0_PEL:4=1}
        verify_pel {$server xgroup delconsumer st g c1} {db0_PEL:1=1}
    }

    test "STREAM-STATS - XGROUP DESTROY removes the sample $suffix" {
        verify_pel {$server FLUSHALL} {}
        seed_stream $server st 4
        $server xgroup create st g1 0
        $server xgroup create st g2 0
        $server xreadgroup group g1 c count 4 streams st >
        verify_pel {} {db0_PEL:0=1,4=1}
        verify_pel {$server xgroup destroy st g1} {db0_PEL:0=1}
        verify_pel {$server xgroup destroy st g2} {}
    }

    test "STREAM-STATS - XDELEX DELREF purges across groups $suffix" {
        verify_pel {$server FLUSHALL} {}
        seed_stream $server st 4
        $server xgroup create st g1 0
        $server xgroup create st g2 0
        $server xreadgroup group g1 c count 4 streams st >
        $server xreadgroup group g2 c count 4 streams st >
        verify_pel {} {db0_PEL:4=2}
        # DELREF removes the entry's PEL references from every group at once.
        verify_pel {$server xdelex st DELREF IDS 2 1-1 2-1} {db0_PEL:2=2}
    }

    test "STREAM-STATS - XADD/XTRIM DELREF purges across groups $suffix" {
        verify_pel {$server FLUSHALL} {}
        seed_stream $server st 8
        $server xgroup create st g1 0
        $server xgroup create st g2 0
        $server xreadgroup group g1 c count 8 streams st >
        $server xreadgroup group g2 c count 8 streams st >
        verify_pel {} {db0_PEL:8=2}
        # Trim with DELREF prunes the trimmed entries' PEL refs in both groups.
        verify_pel {$server xtrim st DELREF maxlen 4} {db0_PEL:4=2}
    }

    test "STREAM-STATS - multiple streams and databases $suffix" {
        verify_pel {$server FLUSHALL} {}
        seed_stream $server sa 2
        seed_stream $server sb 4
        $server xgroup create sa g 0
        $server xgroup create sb g 0
        $server xreadgroup group g c count 2 streams sa >
        $server xreadgroup group g c count 4 streams sb >
        verify_pel {} {db0_PEL:2=1,4=1}
        $server select 5
        seed_stream $server sc 8
        $server xgroup create sc g 0
        $server xreadgroup group g c count 8 streams sc >
        verify_pel {} {db0_PEL:2=1,4=1 db5_PEL:8=1}
        $server select 0
    }

    test "STREAM-STATS - randomized sequence matches keyspace cross-check $suffix" {
        verify_pel {$server FLUSHALL} {}
        for {set s 0} {$s < 6} {incr s} {
            seed_stream $server strm$s [expr {int(rand()*30)+1}]
            $server xgroup create strm$s g0 0
            $server xgroup create strm$s g1 0
            catch {$server xreadgroup group g0 c count [expr {int(rand()*20)}] streams strm$s >}
            catch {$server xreadgroup group g1 c count [expr {int(rand()*20)}] streams strm$s >}
            catch {$server xack strm$s g0 [expr {int(rand()*10)+1}]-1}
        }
        verify_pel {} {__EVAL__ 0}
    }
}

start_server {tags {"external:skip" "needs:debug"} overrides {stream-stats yes}} {
    r select 0

    test_all_stream_stats 0

    test "STREAM-STATS - DEBUG RELOAD reconstructs the histogram from RDB" {
        r FLUSHALL
        seed_stream r st 8
        r xgroup create st g1 0
        r xgroup create st g2 0
        r xreadgroup group g1 c count 8 streams st >
        r xreadgroup group g2 c count 3 streams st >
        r xack st g1 1-1
        set before [get_info_stream_stripped r]
        r DEBUG RELOAD
        assert_equal $before [get_info_stream_stripped r]
        assert_equal [eval_pel_histogram r 0] [get_info_stream_stripped r]
    }

    test "STREAM-STATS - section is empty after the streams are removed" {
        r FLUSHALL
        assert_equal "" [get_info_stream_stripped r]
        seed_stream r st 4
        r xgroup create st g 0
        r xreadgroup group g c count 4 streams st >
        assert_equal "db0_distrib_cgroups_pel:4=1" [get_info_stream_stripped r]
        r del st
        assert_equal "" [get_info_stream_stripped r]
    }

    # Start a replica to verify the histogram is reconstructed via replication.
    start_server {tags {needs:repl external:skip} overrides {stream-stats yes}} {
        set primary [srv -1 client]
        set primary_host [srv -1 host]
        set primary_port [srv -1 port]
        set replica [srv 0 client]

        $replica replicaof $primary_host $primary_port
        wait_for_condition 50 100 { [s 0 role] eq {slave} } else { fail "Replication not started." }

        $primary select 0
        test_all_stream_stats 1
    }
}

# The section is everything-only (in `all`/`everything`, not `default`) and is
# gated on the stream-stats directive.
start_server {tags {"external:skip" "needs:debug"} overrides {stream-stats no}} {
    r select 0

    test "STREAM-STATS - section is not part of default INFO" {
        assert_equal 0 [string match "*# Stream*" [r info]]
        assert_equal 1 [string match "*# Stream*" [r info everything]]
        assert_equal 1 [string match "*# Stream*" [r info stream]]
    }

    test "STREAM-STATS - disabled: section present but carries no lines" {
        r FLUSHALL
        seed_stream r st 4
        r xgroup create st g 0
        r xreadgroup group g c count 4 streams st >
        assert_equal "" [get_info_stream_stripped r]
    }

    test "STREAM-STATS - runtime enable is lazy, reload makes it exact" {
        r FLUSHALL
        seed_stream r st 4
        r xgroup create st g 0
        r xreadgroup group g c count 4 streams st >
        # Enabling at runtime starts from a clean slate (no rescan): the
        # pre-existing group isn't counted until its next change.
        r config set stream-stats yes
        assert_equal "" [get_info_stream_stripped r]
        # A reload rebuilds the gauge exactly from the keyspace.
        r DEBUG RELOAD
        assert_equal "db0_distrib_cgroups_pel:4=1" [get_info_stream_stripped r]
        # Disabling zeroes the histogram so no stale samples linger.
        r config set stream-stats no
        assert_equal "" [get_info_stream_stripped r]
    }
}
