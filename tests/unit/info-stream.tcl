################################################################################
# Test the "INFO stream" section.
#
# The section reports per-database, base-2 logarithmic histograms of stream
# properties, identical in form to the "INFO keysizes" section. One sample per
# consumer group per metric:
#   - distrib_cgroups_pel: the group's pending-entry-list (PEL) size.
#   - distrib_cgroups_lag: the group's backlog of unconsumed messages, bounded
#     by the live stream length (matching XINFO GROUPS lag); groups whose lag is
#     unavailable due to fragmentation (XINFO NULL) contribute no sample.
# Collection is gated on the stream-stats directive and reconstructed exactly
# from RDB / replication.
################################################################################

# Map a value to its histogram bin label, matching the C binning (largest power
# of two <= value; 0 -> "0"; >=1024 rendered with K/M/... suffix).
proc hist_label {v} {
    if {$v == 0} { return 0 }
    set power 1
    while { ($power * 2) <= $v } { set power [expr {$power * 2}] }
    if {$power >= 1048576} { return "[expr {$power / 1048576}]M" }
    if {$power >= 1024}    { return "[expr {$power / 1024}]K" }
    return $power
}

# Whole "INFO stream" section, header and whitespace stripped (used to assert
# the section is entirely empty).
proc get_info_stream_stripped {server} {
    return [string map {
        "# Stream" ""
        " " "" "\n" "" "\r" ""
    } [$server info stream]]
}

# Only the "INFO stream" lines for a given metric field (e.g.
# distrib_cgroups_pel), concatenated and whitespace-stripped -- so a per-metric
# assertion isn't disturbed by the other metrics' lines.
proc get_info_stream_field {server field} {
    set out ""
    foreach line [split [$server info stream] "\n"] {
        set line [string trim $line "\r"]
        if {[string match "db*_$field:*" $line]} { append out $line }
    }
    return $out
}

# Reconstruct a metric's expected histogram for 'dbid' directly from the
# keyspace (the INFO-independent cross-check): for every stream, read the given
# XINFO GROUPS field per group and bin it. A nil field (e.g. lag under
# fragmentation) contributes no sample, matching the live histogram. Returns the
# same canonical form as get_info_stream_field.
proc eval_stream_histogram {server dbid metric xinfo_field} {
    $server select $dbid
    array set bin_counts {}
    foreach key [$server keys *] {
        if {[$server type $key] ne "stream"} continue
        foreach g [$server xinfo groups $key] {
            array set gi $g
            set v $gi($xinfo_field)
            if {$v ne {}} { incr bin_counts([hist_label $v]) }
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
    return "db${dbid}_$metric:[join $out ,]"
}

# Resolve the expected string for metric 'field': the sentinel "__EVAL__ <dbid>"
# reconstructs from the keyspace via XINFO 'xinfo_field'; otherwise the literal,
# with the given short 'placeholder' expanded to the field name.
proc stream_metric_expand {server exp field placeholder xinfo_field} {
    if {[regexp {^__EVAL__\s+(\d+)$} $exp -> dbid]} {
        return [eval_stream_histogram $server $dbid $field $xinfo_field]
    }
    return [string map [list $placeholder $field " " "" "\n" "" "\r" ""] $exp]
}

# Run 'cmd', then assert that metric 'field's INFO stream lines equal 'exp'. In
# replicaMode the assertion is repeated on the replica after it catches up.
proc verify_stream_metric {cmd exp field placeholder xinfo_field waitCond} {
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
        [stream_metric_expand $server $exp $field $placeholder $xinfo_field] eq [get_info_stream_field $server $field]
    } else {
        fail "Expected: `[stream_metric_expand $server $exp $field $placeholder $xinfo_field]` Actual: `[get_info_stream_field $server $field]`. After: $cmd"
    }

    if {$replicaMode eq 1} {
        wait_for_condition 50 50 {
            [stream_metric_expand $server $exp $field $placeholder $xinfo_field] eq [get_info_stream_field $replica $field]
        } else {
            fail "Replica mismatch. Expected: `[stream_metric_expand $server $exp $field $placeholder $xinfo_field]` Actual: `[get_info_stream_field $replica $field]`. After: $cmd"
        }
    }
}

# distrib_cgroups_pel: placeholder "PEL", cross-checked against XINFO 'pending'.
proc verify_pel {cmd exp {waitCond 0}} {
    uplevel 1 [list verify_stream_metric $cmd $exp distrib_cgroups_pel PEL pending $waitCond]
}

# distrib_cgroups_lag: placeholder "LAG", cross-checked against XINFO 'lag'.
proc verify_lag {cmd exp {waitCond 0}} {
    uplevel 1 [list verify_stream_metric $cmd $exp distrib_cgroups_lag LAG lag $waitCond]
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
            verify_pel {$server xreadgroup group g c count $n streams st >} "db0_PEL:[hist_label $n]=1"
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
        # XCLAIM FORCE creates PEL entries for the four (existing) IDs -> PEL=4.
        # Bin boundaries are chosen so the later purge crosses one (4 -> "4",
        # 3 -> "2"), otherwise the shrink would be invisible at this granularity.
        verify_pel {$server xclaim st g c 0 1-1 2-1 3-1 4-1 FORCE} {db0_PEL:4=1}
        # XDEL removes the stream entry but leaves its PEL reference dangling;
        # XDEL does not touch the PEL, so the histogram must stay at 4.
        verify_pel {$server xdel st 1-1} {db0_PEL:4=1}
        # Re-claiming the now-dangling ID purges it from the PEL -> PEL=3, which
        # crosses a bin boundary (4 -> "2"), so the purge is observable.
        verify_pel {$server xclaim st g c2 0 1-1 2-1 3-1 4-1 FORCE} {db0_PEL:2=1}
    }

    test "STREAM-STATS - XAUTOCLAIM purges deleted PEL entries $suffix" {
        verify_pel {$server FLUSHALL} {}
        seed_stream $server st 4
        $server xgroup create st g 0
        verify_pel {$server xreadgroup group g c count 4 streams st >} {db0_PEL:4=1}
        # XDEL leaves the deleted entries' PEL references dangling; it does not
        # touch the PEL, so the histogram must stay at 4.
        verify_pel {$server xdel st 1-1 2-1} {db0_PEL:4=1}
        # XAUTOCLAIM purges the two now-dangling refs -> PEL=2, crossing a bin
        # boundary (4 -> "2"), so the shrink is observable.
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
        verify_pel {$server xreadgroup group g1 c count 4 streams st >} {db0_PEL:0=1,4=1}
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

    test "STREAM-STATS - lag grows on XADD, shrinks on XREADGROUP $suffix" {
        verify_lag {$server FLUSHALL} {}
        seed_stream $server st 4
        # A group created at 0 on a 4-entry stream has lag 4.
        verify_lag {$server xgroup create st g 0} {db0_LAG:4=1}
        # Reading 3 entries lowers lag to 1; reading the rest to 0.
        verify_lag {$server xreadgroup group g c count 3 streams st >} {db0_LAG:1=1}
        verify_lag {$server xreadgroup group g c count 10 streams st >} {db0_LAG:0=1}
        # Producing more raises the caught-up group's lag again.
        verify_lag {$server xadd st 5-1 f v} {db0_LAG:1=1}
        verify_lag {$server xadd st 6-1 f v} {db0_LAG:2=1}
    }

    test "STREAM-STATS - lag is bounded: trimming unread entries lowers it $suffix" {
        verify_lag {$server FLUSHALL} {}
        seed_stream $server st 8
        # Group at 0 that hasn't read: lag 8.
        verify_lag {$server xgroup create st g 0} {db0_LAG:8=1}
        # Trimming to the newest 4 removes 4 unread entries. The backlog is the
        # live window (4), not the raw producer-minus-read (8) -- it crosses a
        # bin (8 -> "4"), and matches XINFO lag. A naive added-read would stay 8.
        verify_lag {$server xtrim st maxlen 4} {db0_LAG:4=1}
        # The group still catches up to 0 by reading the 4 live entries.
        verify_lag {$server xreadgroup group g c count 100 streams st >} {db0_LAG:0=1}
    }

    test "STREAM-STATS - lag with multiple groups $suffix" {
        verify_lag {$server FLUSHALL} {}
        seed_stream $server st 8
        $server xgroup create st g1 0
        $server xgroup create st g2 0
        $server xreadgroup group g1 c count 8 streams st >
        $server xreadgroup group g2 c count 2 streams st >
        # g1 caught up (lag 0); g2 read 2 of 8 -> lag 6 -> bin "4".
        verify_lag {} {db0_LAG:0=1,4=1}
    }

    test "STREAM-STATS - fragmented group (NULL lag) is excluded $suffix" {
        verify_lag {$server FLUSHALL} {}
        seed_stream $server st 5
        $server xgroup create st g 0
        $server xreadgroup group g c count 2 streams st >
        verify_lag {} {db0_LAG:2=1} ;# lag 3 -> "2"
        # Deleting an entry ahead of the group leaves a tombstone ahead, so its
        # lag becomes unavailable (XINFO reports NULL) and it drops out.
        verify_lag {$server xdel st 4-1} {}
    }

    test "STREAM-STATS - XGROUP SETID and DESTROY move the lag sample $suffix" {
        verify_lag {$server FLUSHALL} {}
        seed_stream $server st 8
        verify_lag {$server xgroup create st g 0} {db0_LAG:8=1}
        verify_lag {$server xgroup setid st g $} {db0_LAG:0=1} ;# jump to tail -> lag 0
        verify_lag {$server xgroup setid st g 0} {db0_LAG:8=1} ;# back to head -> lag 8
        verify_lag {$server xgroup destroy st g} {}
    }

    test "STREAM-STATS - lag bin is clamped for out-of-range values $suffix" {
        verify_lag {$server FLUSHALL} {}
        seed_stream $server st 3
        $server xgroup create st g 0
        $server xreadgroup group g c count 1 streams st > ;# read pos in range, small entries_read
        # Unlike key sizes, lag is not physically bounded: XSETID ENTRIESADDED can
        # push entries_added to ~2^63, so lag (added - read) exceeds the histogram
        # range. The bin must clamp to the last bucket ("256P", the top of the 60
        # bins) rather than writing past distrib_cgroups_lag (a heap OOB).
        verify_lag {$server xsetid st 3-1 entriesadded 9223372036854775806} {db0_LAG:256P=1}
        assert_equal PONG [$server ping] ;# no crash / corruption
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
            # Trims and deletes exercise the lag bound and fragmentation (NULL).
            catch {$server xtrim strm$s maxlen [expr {int(rand()*10)}]}
            catch {$server xdel strm$s [expr {int(rand()*15)+1}]-1}
        }
        # Both metrics must match an independent reconstruction from XINFO GROUPS
        # (pending for PEL, lag for LAG) -- across trims/deletes/reads/acks.
        verify_pel {} {__EVAL__ 0}
        verify_lag {} {__EVAL__ 0}
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
        # Both metrics match an independent reconstruction from XINFO GROUPS.
        assert_equal [eval_stream_histogram r 0 distrib_cgroups_pel pending] [get_info_stream_field r distrib_cgroups_pel]
        assert_equal [eval_stream_histogram r 0 distrib_cgroups_lag lag] [get_info_stream_field r distrib_cgroups_lag]
    }

    test "STREAM-STATS - section is empty after the streams are removed" {
        r FLUSHALL
        assert_equal "" [get_info_stream_stripped r]
        seed_stream r st 4
        r xgroup create st g 0
        r xreadgroup group g c count 4 streams st >
        assert_equal "db0_distrib_cgroups_pel:4=1" [get_info_stream_field r distrib_cgroups_pel]
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
        assert_equal "db0_distrib_cgroups_pel:4=1" [get_info_stream_field r distrib_cgroups_pel]
        # Disabling zeroes the histogram so no stale samples linger.
        r config set stream-stats no
        assert_equal "" [get_info_stream_stripped r]
    }
}
