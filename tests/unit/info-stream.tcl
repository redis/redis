################################################################################
# Test the "INFO streams" section.
#
# The section reports per-database, base-2 logarithmic histograms of stream
# properties, identical in form to the "INFO keysizes" section. One sample per
# stream, or per consumer group, per metric:
#   - stream_distrib_streams_cgroups: one sample per STREAM, its consumer-group
#     count. A stream is a sample from birth: with no groups it counts in bin 0.
#   - stream_distrib_cgroups_pel: the group's pending-entry-list (PEL) size.
#   - stream_distrib_cgroups_consumers: the group's consumer count.
#   - stream_distrib_streams_max_lag: one sample per STREAM that has a group
#     whose position is known: the worst group lag, entries_added minus the
#     group's entries_read, capped at the stream length. A group created
#     without ENTRIESREAD, at 0, $ or an explicit id, has an unknown position
#     and no lag until a read pins its counter down (reading the first entry
#     of a tombstone-free stream, or reaching its last entry).
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

# Whole "INFO streams" section, header and whitespace stripped (used to assert
# the section is entirely empty).
proc get_info_stream_stripped {server} {
    return [string map {
        "# Streams" ""
        " " "" "\n" "" "\r" ""
    } [$server info streams]]
}

# Only the "INFO streams" lines for a given metric field (e.g.
# stream_distrib_cgroups_pel), concatenated and whitespace-stripped -- so a per-metric
# assertion isn't disturbed by the other metrics' lines.
proc get_info_stream_field {server field} {
    set out ""
    foreach line [split [$server info streams] "\n"] {
        set line [string trim $line "\r"]
        if {[string match "db*_$field:*" $line]} { append out $line }
    }
    return $out
}

# Reconstruct a metric's expected histogram for 'dbid' directly from the
# keyspace (the INFO-independent cross-check). Per-group metrics: for every
# stream, read the given XINFO GROUPS field per group and bin it; a nil field
# contributes no sample, matching the live histogram. 'pending' is never nil,
# so that branch is unused today; it keeps the helper usable for a metric
# XINFO can report as NULL. The two per-stream metrics are rebuilt from their
# definitions instead: 'groups' bins the group count of XINFO STREAM, and
# 'max_lag' bins the worst lag over the stream's groups of known position,
# min(entries-added - smallest entries-read, length), from XINFO STREAM and
# the raw entries-read of XINFO GROUPS; a stream with no such group has no
# sample. XINFO's own lag field is never used: it estimates a lag for a group
# of unknown position, which the row does not count.
# Returns the same canonical form as get_info_stream_field.
proc eval_stream_histogram {server dbid metric xinfo_field} {
    $server select $dbid
    array set bin_counts {}
    foreach key [$server keys *] {
        if {[$server type $key] ne "stream"} continue
        if {$xinfo_field eq "groups"} {
            # A per-stream metric: one sample per stream, from XINFO STREAM.
            incr bin_counts([hist_label [dict get [$server xinfo stream $key] groups]])
            continue
        }
        if {$xinfo_field eq "max_lag"} {
            set min_read {}
            foreach g [$server xinfo groups $key] {
                array set gi $g
                set er $gi(entries-read)
                if {$er ne {} && ($min_read eq {} || $er < $min_read)} { set min_read $er }
                unset gi
            }
            if {$min_read eq {}} continue
            set info [$server xinfo stream $key]
            set added [dict get $info entries-added]
            set lag [expr {$added > $min_read ? $added - $min_read : 0}]
            set len [dict get $info length]
            if {$lag > $len} { set lag $len }
            incr bin_counts([hist_label $lag])
            continue
        }
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

# stream_distrib_cgroups_pel: placeholder "PEL", cross-checked against XINFO 'pending'.
proc verify_pel {cmd exp {waitCond 0}} {
    uplevel 1 [list verify_stream_metric $cmd $exp stream_distrib_cgroups_pel PEL pending $waitCond]
}

# stream_distrib_cgroups_consumers: placeholder "CONS", cross-checked against
# XINFO 'consumers'.
proc verify_consumers {cmd exp {waitCond 0}} {
    uplevel 1 [list verify_stream_metric $cmd $exp stream_distrib_cgroups_consumers CONS consumers $waitCond]
}

# stream_distrib_streams_cgroups: placeholder "SC"; one sample per stream,
# cross-checked against the 'groups' field of XINFO STREAM.
proc verify_streams_cgroups {cmd exp {waitCond 0}} {
    uplevel 1 [list verify_stream_metric $cmd $exp stream_distrib_streams_cgroups SC groups $waitCond]
}

# stream_distrib_streams_max_lag: placeholder "LAG"; one sample per stream, the
# worst lag over its groups of known position. Cross-checked against a
# reconstruction from XINFO STREAM entries-added / length and XINFO GROUPS
# entries-read (see eval_stream_histogram), never against XINFO's lag field.
proc verify_streams_max_lag {cmd exp {waitCond 0}} {
    uplevel 1 [list verify_stream_metric $cmd $exp stream_distrib_streams_max_lag LAG max_lag $waitCond]
}

# Sum of all bin counts on one INFO histogram line ("" -> 0).
proc hist_total {line} {
    set total 0
    foreach part [split [lindex [split $line ":"] 1] ","] {
        if {$part ne ""} { incr total [lindex [split $part "="] 1] }
    }
    return $total
}

# Number of stream keys in the selected db.
proc count_stream_keys {server} {
    set n 0
    foreach key [$server keys *] { if {[$server type $key] eq "stream"} { incr n } }
    return $n
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

    test "STREAM-STATS - consumers bin boundaries 1,2,4,8,... $suffix" {
        # XGROUP CREATECONSUMER adds exactly one consumer per call, so n calls
        # put the group in the bin for the largest power of two <= n.
        foreach n {1 2 3 4 7 8 15 16} {
            verify_consumers {$server FLUSHALL} {}
            $server xgroup create st g 0 mkstream
            for {set i 1} {$i < $n} {incr i} { $server xgroup createconsumer st g c$i }
            verify_consumers {$server xgroup createconsumer st g c$n} "db0_CONS:[hist_label $n]=1"
        }
    }

    test "STREAM-STATS - a new group has no consumers (bin 0) $suffix" {
        verify_consumers {$server FLUSHALL} {}
        verify_consumers {$server xgroup create st g 0 mkstream} {db0_CONS:0=1}
    }

    test "STREAM-STATS - XREADGROUP creates a consumer only on first sight $suffix" {
        verify_consumers {$server FLUSHALL} {}
        seed_stream $server st 4
        verify_consumers {$server xgroup create st g 0} {db0_CONS:0=1}
        # A first read under a new name creates the consumer: 0 -> 1.
        verify_consumers {$server xreadgroup group g alice count 1 streams st >} {db0_CONS:1=1}
        # The same name again does not; the count stays at 1.
        verify_consumers {$server xreadgroup group g alice count 1 streams st >} {db0_CONS:1=1}
        # A different name does: 1 -> 2.
        verify_consumers {$server xreadgroup group g bob count 1 streams st >} {db0_CONS:2=1}
    }

    test "STREAM-STATS - XGROUP CREATECONSUMER / DELCONSUMER move the sample $suffix" {
        verify_consumers {$server FLUSHALL} {}
        $server xgroup create st g 0 mkstream
        verify_consumers {$server xgroup createconsumer st g c1} {db0_CONS:1=1}
        verify_consumers {$server xgroup createconsumer st g c2} {db0_CONS:2=1}
        # Re-creating an existing consumer is a no-op (replies 0) and must not move it.
        verify_consumers {$server xgroup createconsumer st g c2} {db0_CONS:2=1}
        verify_consumers {$server xgroup delconsumer st g c1} {db0_CONS:1=1}
        verify_consumers {$server xgroup delconsumer st g c2} {db0_CONS:0=1}
    }

    test "STREAM-STATS - XCLAIM and XAUTOCLAIM create the claiming consumer $suffix" {
        verify_consumers {$server FLUSHALL} {}
        seed_stream $server st 4
        $server xgroup create st g 0
        $server xreadgroup group g alice count 4 streams st >
        verify_consumers {} {db0_CONS:1=1}
        # A previously unseen claimer is created: 1 -> 2 crosses into "2".
        verify_consumers {$server xclaim st g bob 0 1-1} {db0_CONS:2=1}
        # Claiming under an existing name creates nothing.
        verify_consumers {$server xclaim st g bob 0 2-1} {db0_CONS:2=1}
        # Park a third consumer so the next creation crosses a bin boundary (3 -> 4).
        $server xgroup createconsumer st g carol
        verify_consumers {$server xautoclaim st g dave 0 0 count 1} {db0_CONS:4=1}
    }

    test "STREAM-STATS - XGROUP DESTROY removes the consumers sample $suffix" {
        verify_consumers {$server FLUSHALL} {}
        $server xgroup create st g1 0 mkstream
        $server xgroup create st g2 0
        verify_consumers {$server xgroup createconsumer st g1 c1} {db0_CONS:0=1,1=1}
        verify_consumers {$server xgroup destroy st g1} {db0_CONS:0=1}
        verify_consumers {$server xgroup destroy st g2} {}
    }

    test "STREAM-STATS - XACK does not touch the consumer count $suffix" {
        verify_consumers {$server FLUSHALL} {}
        seed_stream $server st 4
        $server xgroup create st g 0
        verify_consumers {$server xreadgroup group g c count 4 streams st >} {db0_CONS:1=1}
        verify_pel {$server xack st g 1-1 2-1} {db0_PEL:2=1}
        verify_consumers {} {db0_CONS:1=1}
    }

    test "STREAM-STATS - streams_cgroups bin boundaries 1,2,4,8,... $suffix" {
        foreach n {1 2 3 4 7 8 15 16} {
            verify_streams_cgroups {$server FLUSHALL} {}
            seed_stream $server st 1
            for {set i 1} {$i < $n} {incr i} { $server xgroup create st g$i 0 }
            verify_streams_cgroups {$server xgroup create st g$n 0} "db0_SC:[hist_label $n]=1"
        }
    }

    test "STREAM-STATS - a stream is a sample from birth; no groups is bin 0 $suffix" {
        verify_streams_cgroups {$server FLUSHALL} {}
        # The XADD that creates the key enters the stream with 0 groups...
        verify_streams_cgroups {$server xadd st 1-1 f v} {db0_SC:0=1}
        # ...further XADDs change nothing...
        verify_streams_cgroups {$server xadd st 2-1 f v} {db0_SC:0=1}
        # ...groups move it up...
        verify_streams_cgroups {$server xgroup create st g1 0} {db0_SC:1=1}
        verify_streams_cgroups {$server xgroup create st g2 0} {db0_SC:2=1}
        # ...destroying them walks it back down to bin 0, not out of the row...
        verify_streams_cgroups {$server xgroup destroy st g1} {db0_SC:1=1}
        verify_streams_cgroups {$server xgroup destroy st g2} {db0_SC:0=1}
        # ...and only deleting the key removes the sample.
        verify_streams_cgroups {$server del st} {}
    }

    test "STREAM-STATS - XGROUP CREATE MKSTREAM births the stream with one group $suffix" {
        verify_streams_cgroups {$server FLUSHALL} {}
        verify_streams_cgroups {$server xgroup create st g 0 mkstream} {db0_SC:1=1}
        # A duplicate group is rejected (BUSYGROUP) and must not move the sample.
        catch {$server xgroup create st g 0}
        verify_streams_cgroups {} {db0_SC:1=1}
    }

    test "STREAM-STATS - streams with and without groups side by side $suffix" {
        verify_streams_cgroups {$server FLUSHALL} {}
        seed_stream $server a 1
        seed_stream $server b 1
        seed_stream $server c 1
        $server xgroup create c g1 0
        $server xgroup create c g2 0
        verify_streams_cgroups {$server xgroup create c g3 0} {db0_SC:0=2,2=1}
        # Every stream is exactly one sample, so the row totals the stream keys.
        assert_equal 3 [hist_total [get_info_stream_field $server stream_distrib_streams_cgroups]]
    }

    test "STREAM-STATS - group-level traffic leaves the stream sample alone $suffix" {
        verify_streams_cgroups {$server FLUSHALL} {}
        seed_stream $server st 4
        verify_streams_cgroups {$server xgroup create st g 0} {db0_SC:1=1}
        $server xreadgroup group g c count 4 streams st >
        $server xack st g 1-1
        $server xgroup createconsumer st g c2
        $server xclaim st g c3 0 2-1
        verify_streams_cgroups {$server xgroup delconsumer st g c2} {db0_SC:1=1}
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
        verify_consumers {} {db0_CONS:1=2 db5_CONS:1=1}
        verify_streams_cgroups {} {db0_SC:1=2 db5_SC:1=1}
        $server select 0
    }

    test "STREAM-STATS - streams_max_lag: XADD grows the worst lag across bins 1,2,4,8 $suffix" {
        verify_streams_max_lag {$server FLUSHALL} {}
        seed_stream $server st 1
        # A group created at 0 has an unknown position: no sample yet...
        verify_streams_max_lag {$server xgroup create st g 0} {}
        # ...its first read pins the position (entries_read 1), lag 0.
        verify_streams_max_lag {$server xreadgroup group g c count 1 streams st >} {db0_LAG:0=1}
        # Every XADD now adds one to the lag; the sample moves at 1, 2, 4, 8.
        for {set i 2} {$i <= 9} {incr i} {
            verify_streams_max_lag {$server xadd st $i-1 f v} "db0_LAG:[hist_label [expr {$i - 1}]]=1"
        }
    }

    test "STREAM-STATS - streams_max_lag: the laggiest group sets the sample, reads lower it $suffix" {
        verify_streams_max_lag {$server FLUSHALL} {}
        seed_stream $server st 8
        $server xgroup create st g1 0
        $server xgroup create st g2 0
        # g1 catches up: lag 0. g2 is still unknown, so it does not count.
        verify_streams_max_lag {$server xreadgroup group g1 c count 8 streams st >} {db0_LAG:0=1}
        # g2 reads 2 of 8: its lag of 6 is now the stream's worst.
        verify_streams_max_lag {$server xreadgroup group g2 c count 2 streams st >} {db0_LAG:4=1}
        verify_streams_max_lag {$server xreadgroup group g2 c count 4 streams st >} {db0_LAG:2=1}
        verify_streams_max_lag {$server xreadgroup group g2 c count 2 streams st >} {db0_LAG:0=1}
    }

    test "STREAM-STATS - streams_max_lag: XGROUP DESTROY of the laggiest group re-elects $suffix" {
        verify_streams_max_lag {$server FLUSHALL} {}
        seed_stream $server st 8
        $server xgroup create st g1 0
        $server xgroup create st g2 0
        $server xgroup create st g3 0
        $server xreadgroup group g1 c count 8 streams st >   ;# lag 0
        $server xreadgroup group g2 c count 1 streams st >   ;# lag 7
        verify_streams_max_lag {$server xreadgroup group g3 c count 6 streams st >} {db0_LAG:4=1}   ;# lag 2
        verify_streams_max_lag {$server xgroup destroy st g2} {db0_LAG:2=1}
        verify_streams_max_lag {$server xgroup destroy st g3} {db0_LAG:0=1}
        # Without groups a stream has no lag to report: no sample, not bin 0.
        verify_streams_max_lag {$server xgroup destroy st g1} {}
    }

    test "STREAM-STATS - streams_max_lag: trims and deletes cap the lag at the length $suffix" {
        verify_streams_max_lag {$server FLUSHALL} {}
        seed_stream $server st 8
        $server xgroup create st g 0
        verify_streams_max_lag {$server xreadgroup group g c count 1 streams st >} {db0_LAG:4=1}   ;# lag 7
        # The group is 7 behind, but only 2 entries are left to deliver.
        verify_streams_max_lag {$server xtrim st maxlen 2} {db0_LAG:2=1}
        verify_streams_max_lag {$server xdel st 7-1 8-1} {db0_LAG:0=1}
        verify_streams_max_lag {$server xadd st 9-1 f v} {db0_LAG:1=1}
    }

    test "STREAM-STATS - streams_max_lag: an unknown position is not a lag $suffix" {
        verify_streams_max_lag {$server FLUSHALL} {}
        seed_stream $server st 8
        # Created at an explicit id without ENTRIESREAD: unknown, no sample.
        verify_streams_max_lag {$server xgroup create st g 4-1} {}
        # A read that stops short of the last entry cannot pin the counter...
        verify_streams_max_lag {$server xreadgroup group g c count 1 streams st >} {}
        # ...reaching the last entry does: entries_read 8, lag 0.
        verify_streams_max_lag {$server xreadgroup group g c count 3 streams st >} {db0_LAG:0=1}
        # A group created at $ is unknown too, so it does not count...
        verify_streams_max_lag {$server xgroup create st g2 $} {db0_LAG:0=1}
        verify_streams_max_lag {$server xadd st 9-1 f v} {db0_LAG:1=1}
        # ...until it reads to the tip; then g, one behind, is the worst.
        verify_streams_max_lag {$server xreadgroup group g2 c count 1 streams st >} {db0_LAG:1=1}
        verify_streams_max_lag {$server xreadgroup group g c count 1 streams st >} {db0_LAG:0=1}
    }

    test "STREAM-STATS - streams_max_lag: XSETID ENTRIESADDED moves the sample and clamps the groups $suffix" {
        verify_streams_max_lag {$server FLUSHALL} {}
        seed_stream $server st 8
        $server xgroup create st g1 0
        $server xgroup create st g2 0
        verify_streams_max_lag {$server xreadgroup group g1 c count 8 streams st >} {db0_LAG:0=1}
        verify_streams_max_lag {$server xreadgroup group g2 c count 4 streams st >} {db0_LAG:4=1}
        # Two entries left: the cap by length brings g2's lag of 4 down to 2.
        verify_streams_max_lag {$server xtrim st maxlen 2} {db0_LAG:2=1}
        # Lowering entries_added to 3, below both read counts (8 and 4), clamps
        # them to 3: lag 0 for both, with no group command involved.
        verify_streams_max_lag {$server xsetid st 8-1 entriesadded 3} {db0_LAG:0=1}
        # Raising it moves the sample up again, capped by the two entries.
        verify_streams_max_lag {$server xsetid st 8-1 entriesadded 10} {db0_LAG:2=1}
        # g1 moves ahead to lag 1; g2 has read 3 of 10, lag 7 capped to the two
        # entries, and stays the laggiest: the row holds.
        verify_streams_max_lag {$server xgroup setid st g1 8-1 entriesread 9} {db0_LAG:2=1}
        # A partial clamp: only g1 has read more than 4 and is clamped to it
        # (lag 0); g2 is untouched, its lag shrinks to 1 with entries_added.
        verify_streams_max_lag {$server xsetid st 8-1 entriesadded 4} {db0_LAG:1=1}
        # Both lags back at the cap of 2.
        verify_streams_max_lag {$server xsetid st 8-1 entriesadded 10} {db0_LAG:2=1}
        # Destroying g1, which is not the laggiest, leaves the sample in place.
        verify_streams_max_lag {$server xgroup destroy st g1} {db0_LAG:2=1}
        # Lowering without a clamp: g2 has read 3, below 4; only its lag moves.
        verify_streams_max_lag {$server xsetid st 8-1 entriesadded 4} {db0_LAG:1=1}
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
            # Trims and deletes must leave the PEL alone -- they only leave
            # dangling references behind -- so they check the histogram stays put.
            catch {$server xtrim strm$s maxlen [expr {int(rand()*10)}]}
            catch {$server xdel strm$s [expr {int(rand()*15)+1}]-1}
        }
        # All four metrics must match an independent reconstruction from XINFO
        # (GROUPS pending / consumers / entries-read, STREAM groups /
        # entries-added / length) -- across trims/deletes/reads/acks.
        verify_pel {} {__EVAL__ 0}
        verify_consumers {} {__EVAL__ 0}
        verify_streams_cgroups {} {__EVAL__ 0}
        verify_streams_max_lag {} {__EVAL__ 0}
    }

    test "STREAM-STATS - streams_max_lag: 520 groups under random operations $suffix" {
        # One stream with about 520 groups: a heap of ten levels whose count
        # drifts across 512, so sifts, removals and inserts meet both a full
        # tree and a partial bottom level. The stream is short, so positions
        # collide and equal keys stop sifts early. The armed validator checks
        # the heap against every group's stamp after each command; the
        # cross-check at the end compares the row with the keyspace. Set
        # STREAM_LAG_SEED to replay a run.
        verify_streams_max_lag {$server FLUSHALL} {}
        set seed [expr {[clock microseconds] % 2147483647}]
        if {[info exists ::env(STREAM_LAG_SEED)]} { set seed $::env(STREAM_LAG_SEED) }
        expr {srand($seed)}
        if {[catch {
            set groups 520
            set added 100
            seed_stream $server st $added
            set live {}
            for {set g 0} {$g < $groups} {incr g} {
                $server xgroup create st g$g 0
                $server xreadgroup group g$g c count [expr {int(rand()*$added)+1}] streams st >
                lappend live g$g
            }
            set next $groups
            for {set i 0} {$i < 2000} {incr i} {
                set g [lindex $live [expr {int(rand()*[llength $live])}]]
                set op [expr {int(rand()*100)}]
                if {$op < 35} {
                    # Re-key down by reading.
                    $server xreadgroup group $g c count [expr {int(rand()*20)+1}] streams st >
                } elseif {$op < 50} {
                    # Re-key either way; a value above entries_added is clamped.
                    $server xgroup setid st $g [expr {int(rand()*$added)}]-1 entriesread [expr {int(rand()*($added+5))}]
                } elseif {$op < 55} {
                    # Unknown position: the key becomes the sentinel and sinks.
                    $server xgroup setid st $g [expr {int(rand()*$added)}]-1
                } elseif {$op < 65} {
                    # Equal destroy and create weights: the count walks across 512.
                    $server xgroup destroy st $g
                    set live [lsearch -all -inline -not -exact $live $g]
                } elseif {$op < 75} {
                    # A fresh group at 0 is the laggiest: a full-depth sift up.
                    $server xgroup create st g$next 0
                    lappend live g$next
                    incr next
                } elseif {$op < 85} {
                    # Threshold crossings.
                    incr added
                    $server xadd st $added-1 f v
                } elseif {$op < 90} {
                    $server xdel st [expr {int(rand()*$added)+1}]-1
                } elseif {$op < 95} {
                    $server xtrim st maxlen [expr {int(rand()*$added)}]
                } else {
                    # Lower entries_added towards the length: clamps every
                    # group above it with no group command involved.
                    set info [$server xinfo stream st]
                    set len [dict get $info length]
                    set ea [dict get $info entries-added]
                    $server xsetid st [dict get $info last-generated-id] entriesadded [expr {$len + int(rand()*($ea - $len + 1))}]
                }
            }
            verify_pel {} {__EVAL__ 0}
            verify_consumers {} {__EVAL__ 0}
            verify_streams_cgroups {} {__EVAL__ 0}
            verify_streams_max_lag {} {__EVAL__ 0}
        } err]} { error "seed $seed: $err" }
    }
}

start_server {tags {"external:skip" "needs:debug"} overrides {stream-stats yes}} {
    r select 0
    # Rebuild the stream histograms from the keyspace after every command and
    # panic on any disagreement, so each test below also covers the bookkeeping:
    # a missed update site, or one computed against mismatched state, fails
    # immediately.
    r debug stream-stats-assert 1

    test_all_stream_stats 0

    # createComplexDataset drives streams with consumer groups through random
    # XADD / XADD MAXLEN / XTRIM / XREADGROUP / XACKDEL / XDEL sequences, i.e. the
    # PEL traffic nobody hand-wrote a case for. Two checks run here: the
    # armed DEBUG STREAM-STATS-ASSERT rebuilds after every one of those commands,
    # and the cross-check below compares the final histograms with XINFO GROUPS.
    test "STREAM-STATS - Test complex dataset" {
        verify_pel {r FLUSHALL} {}
        createComplexDataset r 1000
        verify_pel {} {__EVAL__ 0}
        verify_consumers {} {__EVAL__ 0}
        verify_streams_cgroups {} {__EVAL__ 0}
        verify_streams_max_lag {} {__EVAL__ 0}
        # Every stream is exactly one sample of the groups-per-stream row.
        assert_equal [count_stream_keys r] [hist_total [get_info_stream_field r stream_distrib_streams_cgroups]]

        # A reload must reconstruct all four metrics for a random dataset too.
        set before_pel [get_info_stream_field r stream_distrib_cgroups_pel]
        set before_cons [get_info_stream_field r stream_distrib_cgroups_consumers]
        set before_sc [get_info_stream_field r stream_distrib_streams_cgroups]
        set before_lag [get_info_stream_field r stream_distrib_streams_max_lag]
        r DEBUG RELOAD
        assert_equal $before_pel [get_info_stream_field r stream_distrib_cgroups_pel]
        assert_equal $before_cons [get_info_stream_field r stream_distrib_cgroups_consumers]
        assert_equal $before_sc [get_info_stream_field r stream_distrib_streams_cgroups]
        assert_equal $before_lag [get_info_stream_field r stream_distrib_streams_max_lag]

        verify_pel {r FLUSHALL} {}
        createComplexDataset r 1000 {useexpire}
        verify_pel {} {__EVAL__ 0}
        verify_consumers {} {__EVAL__ 0}
        verify_streams_cgroups {} {__EVAL__ 0}
        verify_streams_max_lag {} {__EVAL__ 0}
    } {} {cluster:skip}

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
        # All four metrics match an independent reconstruction from XINFO.
        assert_equal [eval_stream_histogram r 0 stream_distrib_cgroups_pel pending] [get_info_stream_field r stream_distrib_cgroups_pel]
        assert_equal [eval_stream_histogram r 0 stream_distrib_cgroups_consumers consumers] \
            [get_info_stream_field r stream_distrib_cgroups_consumers]
        assert_equal [eval_stream_histogram r 0 stream_distrib_streams_cgroups groups] \
            [get_info_stream_field r stream_distrib_streams_cgroups]
        assert_equal [eval_stream_histogram r 0 stream_distrib_streams_max_lag max_lag] \
            [get_info_stream_field r stream_distrib_streams_max_lag]
    }

    test "STREAM-STATS - streams_max_lag: a stream that appears whole enrols every group at once" {
        r FLUSHALL
        seed_stream r st 8
        r xgroup create st g1 0
        r xgroup create st g2 0
        r xreadgroup group g1 c count 8 streams st >
        r xreadgroup group g2 c count 2 streams st >
        assert_equal "db0_stream_distrib_streams_max_lag:4=1" [get_info_stream_field r stream_distrib_streams_max_lag]
        # COPY and RESTORE build the new stream's heap from all its groups in
        # one go; RENAME is a removal and an appearance under another name.
        r copy st st2
        assert_equal "db0_stream_distrib_streams_max_lag:4=2" [get_info_stream_field r stream_distrib_streams_max_lag]
        r restore st3 0 [r dump st]
        assert_equal "db0_stream_distrib_streams_max_lag:4=3" [get_info_stream_field r stream_distrib_streams_max_lag]
        r rename st3 st4
        assert_equal "db0_stream_distrib_streams_max_lag:4=3" [get_info_stream_field r stream_distrib_streams_max_lag]
        # MOVE takes the sample along. INFO lists db 0 then db 1, and the
        # helper concatenates the two lines.
        r move st2 1
        assert_equal "db0_stream_distrib_streams_max_lag:4=2db1_stream_distrib_streams_max_lag:4=1" [get_info_stream_field r stream_distrib_streams_max_lag]
        # The rebuilt heap is live: a read by the moved copy's laggard lowers it.
        r select 1
        r xreadgroup group g2 c count 4 streams st2 >
        assert_equal "db0_stream_distrib_streams_max_lag:4=2db1_stream_distrib_streams_max_lag:2=1" [get_info_stream_field r stream_distrib_streams_max_lag]
        r flushdb
        r select 0
        r del st4
        assert_equal "db0_stream_distrib_streams_max_lag:4=1" [get_info_stream_field r stream_distrib_streams_max_lag]
        assert_equal [eval_stream_histogram r 0 stream_distrib_streams_max_lag max_lag] [get_info_stream_field r stream_distrib_streams_max_lag]
    } {} {cluster:skip}

    test "STREAM-STATS - section is empty after the streams are removed" {
        r FLUSHALL
        assert_equal "" [get_info_stream_stripped r]
        seed_stream r st 4
        r xgroup create st g 0
        r xreadgroup group g c count 4 streams st >
        assert_equal "db0_stream_distrib_cgroups_pel:4=1" [get_info_stream_field r stream_distrib_cgroups_pel]
        assert_equal "db0_stream_distrib_cgroups_consumers:1=1" [get_info_stream_field r stream_distrib_cgroups_consumers]
        assert_equal "db0_stream_distrib_streams_cgroups:1=1" [get_info_stream_field r stream_distrib_streams_cgroups]
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
        # Arm the assertion on both sides: the replica maintains its histograms
        # from the propagated commands, which is a separate path.
        $primary debug stream-stats-assert 1
        $replica debug stream-stats-assert 1
        test_all_stream_stats 1
    }
}

# The section is in `all`/`everything` and by name, not in `default`, and is
# gated on the stream-stats directive.
start_server {tags {"external:skip" "needs:debug"} overrides {stream-stats no}} {
    r select 0

    test "STREAM-STATS - section is not part of default INFO" {
        assert_equal 0 [string match "*# Streams*" [r info]]
        assert_equal 1 [string match "*# Streams*" [r info everything]]
        assert_equal 1 [string match "*# Streams*" [r info streams]]
    }

    test "STREAM-STATS - disabled: section present but carries no lines" {
        r FLUSHALL
        seed_stream r st 4
        r xgroup create st g 0
        r xreadgroup group g c count 4 streams st >
        assert_equal "" [get_info_stream_stripped r]
    }

    # The lazy-enable tests below run with the assertion unarmed on purpose:
    # re-enabling stream-stats while armed primes a full rebuild (see
    # applyStreamStats), which would register every object and hide exactly the
    # pre-existing / never-counted state these tests exercise.

    test "STREAM-STATS - lazy enable: a first update registers the object and steals nothing" {
        r config set stream-stats no
        r FLUSHALL
        seed_stream r A 4
        r xgroup create A gA 0
        seed_stream r B 4
        r xgroup create B gB 0
        r config set stream-stats yes
        # Nothing is counted yet: both groups predate the enable.
        assert_equal "" [get_info_stream_stripped r]
        # gB's first read enters its consumers sample (the new consumer) and its
        # PEL sample (at 1); the ack then moves the PEL 1 -> 0.
        r xreadgroup group gB c count 1 streams B >
        r xack B gB 1-1
        assert_equal "db0_stream_distrib_cgroups_pel:0=1" [get_info_stream_field r stream_distrib_cgroups_pel]
        assert_equal "db0_stream_distrib_cgroups_consumers:1=1" [get_info_stream_field r stream_distrib_cgroups_consumers]
        # The read pinned gB's position at the first entry: lag 3, bin "2".
        assert_equal "db0_stream_distrib_streams_max_lag:2=1" [get_info_stream_field r stream_distrib_streams_max_lag]
        # gA's first change used to decrement bin 0 -- gB's tally -- because gA
        # was never in it. Now it registers gA instead: 0=1 (gB) and 1=1 (gA).
        r xreadgroup group gA c count 1 streams A >
        assert_equal "db0_stream_distrib_cgroups_pel:0=1,1=1" [get_info_stream_field r stream_distrib_cgroups_pel]
        assert_equal "db0_stream_distrib_cgroups_consumers:1=2" [get_info_stream_field r stream_distrib_cgroups_consumers]
        assert_equal "db0_stream_distrib_streams_max_lag:2=2" [get_info_stream_field r stream_distrib_streams_max_lag]
    }

    test "STREAM-STATS - lazy enable: removing a never-counted stream steals nothing" {
        r config set stream-stats no
        r FLUSHALL
        seed_stream r A 4
        r xgroup create A gA 0
        seed_stream r B 4
        r xgroup create B gB 0
        r config set stream-stats yes
        r xreadgroup group gB c count 1 streams B >
        r xack B gB 1-1
        assert_equal "db0_stream_distrib_cgroups_pel:0=1" [get_info_stream_field r stream_distrib_cgroups_pel]
        # DEL of a stream whose group was never counted must remove nothing.
        r del A
        assert_equal "db0_stream_distrib_cgroups_pel:0=1" [get_info_stream_field r stream_distrib_cgroups_pel]
        assert_equal "db0_stream_distrib_cgroups_consumers:1=1" [get_info_stream_field r stream_distrib_cgroups_consumers]
        assert_equal "db0_stream_distrib_streams_max_lag:2=1" [get_info_stream_field r stream_distrib_streams_max_lag]
    }

    test "STREAM-STATS - lazy enable: a two-metric command on a stale group is exact" {
        # XCLAIM and XGROUP DELCONSUMER update PEL and consumers in one command.
        # Each row enters the group's sample from its own new value, so no
        # untrusted old value is ever decremented and the result is exact.
        r config set stream-stats no
        r FLUSHALL
        seed_stream r A 4
        r xgroup create A gA 0
        r xreadgroup group gA alice count 4 streams A >
        seed_stream r B 4
        r xgroup create B gB 0
        r xreadgroup group gB bob count 2 streams B >
        r xgroup createconsumer B gB carol
        r config set stream-stats yes
        # A new claimer on stale gA: PEL stays 4, consumers 1 -> 2.
        r xclaim A gA zed 0 1-1
        assert_equal "db0_stream_distrib_cgroups_pel:4=1" [get_info_stream_field r stream_distrib_cgroups_pel]
        assert_equal "db0_stream_distrib_cgroups_consumers:2=1" [get_info_stream_field r stream_distrib_cgroups_consumers]
        assert_equal "db0_stream_distrib_streams_max_lag:0=1" [get_info_stream_field r stream_distrib_streams_max_lag]
        # DELCONSUMER on stale gB: PEL stays 2, consumers 2 -> 1.
        r xgroup delconsumer B gB carol
        assert_equal "db0_stream_distrib_cgroups_pel:2=1,4=1" [get_info_stream_field r stream_distrib_cgroups_pel]
        assert_equal "db0_stream_distrib_cgroups_consumers:1=1,2=1" [get_info_stream_field r stream_distrib_cgroups_consumers]
        assert_equal "db0_stream_distrib_streams_max_lag:0=1,2=1" [get_info_stream_field r stream_distrib_streams_max_lag]
        # Both groups are now counted, and the rows match the keyspace exactly.
        assert_equal [eval_stream_histogram r 0 stream_distrib_cgroups_pel pending] [get_info_stream_field r stream_distrib_cgroups_pel]
        assert_equal [eval_stream_histogram r 0 stream_distrib_cgroups_consumers consumers] [get_info_stream_field r stream_distrib_cgroups_consumers]
        assert_equal [eval_stream_histogram r 0 stream_distrib_streams_max_lag max_lag] [get_info_stream_field r stream_distrib_streams_max_lag]
    }

    test "STREAM-STATS - lazy enable: a change within the same bin registers a stale group" {
        r config set stream-stats no
        r FLUSHALL
        seed_stream r A 8
        r xgroup create A g 0
        r xreadgroup group g c count 5 streams A >
        r config set stream-stats yes
        # PEL 5 -> 4 stays in bin "4". Before, a same-bin change was an early
        # return and the group stayed invisible; now the change enters its sample.
        r xack A g 1-1
        assert_equal "db0_stream_distrib_cgroups_pel:4=1" [get_info_stream_field r stream_distrib_cgroups_pel]
        assert_equal "db0_stream_distrib_streams_max_lag:2=1" [get_info_stream_field r stream_distrib_streams_max_lag]
    }

    test "STREAM-STATS - lazy enable: a read by an existing consumer registers every row" {
        # A steady workload is reads and acks by consumers that already exist;
        # it must fill in all three rows, not only the PEL one.
        r config set stream-stats no
        r FLUSHALL
        r xadd s 1-0 f v
        r xgroup create s g 0
        r xreadgroup group g c streams s >
        r config set stream-stats yes
        assert_equal "" [get_info_stream_stripped r]
        r xadd s 2-0 f v
        r xreadgroup group g c streams s >
        assert_equal "db0_stream_distrib_streams_cgroups:1=1" [get_info_stream_field r stream_distrib_streams_cgroups]
        assert_equal "db0_stream_distrib_cgroups_pel:2=1" [get_info_stream_field r stream_distrib_cgroups_pel]
        assert_equal "db0_stream_distrib_cgroups_consumers:1=1" [get_info_stream_field r stream_distrib_cgroups_consumers]
        assert_equal [eval_stream_histogram r 0 stream_distrib_streams_cgroups groups] [get_info_stream_field r stream_distrib_streams_cgroups]
        assert_equal [eval_stream_histogram r 0 stream_distrib_cgroups_consumers consumers] [get_info_stream_field r stream_distrib_cgroups_consumers]
        assert_equal "db0_stream_distrib_streams_max_lag:0=1" [get_info_stream_field r stream_distrib_streams_max_lag]
        assert_equal [eval_stream_histogram r 0 stream_distrib_streams_max_lag max_lag] [get_info_stream_field r stream_distrib_streams_max_lag]
    }

    test "STREAM-STATS - lazy enable: an ack registers every row of the group and its stream" {
        r config set stream-stats no
        r FLUSHALL
        seed_stream r s 4
        r xgroup create s g 0
        r xreadgroup group g c count 2 streams s >
        r config set stream-stats yes
        r xack s g 1-1
        assert_equal "db0_stream_distrib_streams_cgroups:1=1" [get_info_stream_field r stream_distrib_streams_cgroups]
        assert_equal "db0_stream_distrib_cgroups_pel:1=1" [get_info_stream_field r stream_distrib_cgroups_pel]
        assert_equal "db0_stream_distrib_cgroups_consumers:1=1" [get_info_stream_field r stream_distrib_cgroups_consumers]
        assert_equal "db0_stream_distrib_streams_max_lag:2=1" [get_info_stream_field r stream_distrib_streams_max_lag]
    }

    test "STREAM-STATS - lazy enable: a read that delivers nothing registers every row" {
        # Workers blocked or polling on an idle group never reach the serving
        # loop; the registration must not depend on an entry being delivered.
        r config set stream-stats no
        r FLUSHALL
        seed_stream r s 3
        r xgroup create s g 0
        r xreadgroup group g c count 3 streams s >
        r config set stream-stats yes
        assert_equal "" [get_info_stream_stripped r]
        assert_equal {} [r xreadgroup group g c2 streams s >]
        assert_equal "db0_stream_distrib_streams_cgroups:1=1" [get_info_stream_field r stream_distrib_streams_cgroups]
        assert_equal "db0_stream_distrib_cgroups_pel:2=1" [get_info_stream_field r stream_distrib_cgroups_pel]
        assert_equal "db0_stream_distrib_cgroups_consumers:2=1" [get_info_stream_field r stream_distrib_cgroups_consumers]
        # The group has read everything: it enrols with lag 0.
        assert_equal "db0_stream_distrib_streams_max_lag:0=1" [get_info_stream_field r stream_distrib_streams_max_lag]
    }

    test "STREAM-STATS - lazy enable: XGROUP CREATECONSUMER registers every row, backlog included" {
        # The group has a backlog nobody reads; administering it must still
        # enter its PEL sample, or the row would read as "no backlog".
        r config set stream-stats no
        r FLUSHALL
        seed_stream r s 4
        r xgroup create s g 0
        r xreadgroup group g c count 3 streams s >
        r config set stream-stats yes
        assert_equal "" [get_info_stream_stripped r]
        r xgroup createconsumer s g c2
        assert_equal "db0_stream_distrib_streams_cgroups:1=1" [get_info_stream_field r stream_distrib_streams_cgroups]
        assert_equal "db0_stream_distrib_cgroups_pel:2=1" [get_info_stream_field r stream_distrib_cgroups_pel]
        assert_equal "db0_stream_distrib_cgroups_consumers:2=1" [get_info_stream_field r stream_distrib_cgroups_consumers]
        assert_equal "db0_stream_distrib_streams_max_lag:1=1" [get_info_stream_field r stream_distrib_streams_max_lag]
    }

    test "STREAM-STATS - lazy enable: XGROUP DELCONSUMER registers every row" {
        r config set stream-stats no
        r FLUSHALL
        seed_stream r s 4
        r xgroup create s g 0
        r xreadgroup group g c1 count 1 streams s >
        r xreadgroup group g c2 count 2 streams s >
        r config set stream-stats yes
        assert_equal "" [get_info_stream_stripped r]
        r xgroup delconsumer s g c1
        assert_equal "db0_stream_distrib_streams_cgroups:1=1" [get_info_stream_field r stream_distrib_streams_cgroups]
        assert_equal "db0_stream_distrib_cgroups_pel:2=1" [get_info_stream_field r stream_distrib_cgroups_pel]
        assert_equal "db0_stream_distrib_cgroups_consumers:1=1" [get_info_stream_field r stream_distrib_cgroups_consumers]
        assert_equal "db0_stream_distrib_streams_max_lag:1=1" [get_info_stream_field r stream_distrib_streams_max_lag]
    }

    test "STREAM-STATS - lazy enable: XGROUP SETID registers every row" {
        r config set stream-stats no
        r FLUSHALL
        seed_stream r s 4
        r xgroup create s g 0
        r xreadgroup group g c count 2 streams s >
        r config set stream-stats yes
        assert_equal "" [get_info_stream_stripped r]
        r xgroup setid s g 0
        assert_equal "db0_stream_distrib_streams_cgroups:1=1" [get_info_stream_field r stream_distrib_streams_cgroups]
        assert_equal "db0_stream_distrib_cgroups_pel:2=1" [get_info_stream_field r stream_distrib_cgroups_pel]
        assert_equal "db0_stream_distrib_cgroups_consumers:1=1" [get_info_stream_field r stream_distrib_cgroups_consumers]
        # SETID without ENTRIESREAD makes the position unknown: the group
        # enrols, but an unknown position is not a lag, so no sample...
        assert_equal "" [get_info_stream_field r stream_distrib_streams_max_lag]
        # ...and with ENTRIESREAD 0 the lag is the whole stream, capped at 4.
        r xgroup setid s g 0 entriesread 0
        assert_equal "db0_stream_distrib_streams_max_lag:4=1" [get_info_stream_field r stream_distrib_streams_max_lag]
    }

    test "STREAM-STATS - lazy enable: XNACK registers every row" {
        r config set stream-stats no
        r FLUSHALL
        seed_stream r s 4
        r xgroup create s g 0
        r xreadgroup group g c count 2 streams s >
        r config set stream-stats yes
        assert_equal "" [get_info_stream_stripped r]
        r xnack s g SILENT IDS 2 1-1 2-1
        assert_equal "db0_stream_distrib_streams_cgroups:1=1" [get_info_stream_field r stream_distrib_streams_cgroups]
        assert_equal "db0_stream_distrib_cgroups_pel:2=1" [get_info_stream_field r stream_distrib_cgroups_pel]
        assert_equal "db0_stream_distrib_cgroups_consumers:1=1" [get_info_stream_field r stream_distrib_cgroups_consumers]
        assert_equal "db0_stream_distrib_streams_max_lag:2=1" [get_info_stream_field r stream_distrib_streams_max_lag]
    }

    test "STREAM-STATS - lazy enable: the lag row sees a group only once it is touched" {
        # Membership in the stream's heap is lazy per group, like every row's
        # sample. Until the laggard is touched, the stream's sample is the worst
        # over the members it has, not over all its groups.
        r config set stream-stats no
        r FLUSHALL
        seed_stream r s 8
        r xgroup create s g1 0
        r xgroup create s g2 0
        r xreadgroup group g1 c count 8 streams s >
        r xreadgroup group g2 c count 2 streams s >
        r config set stream-stats yes
        assert_equal "" [get_info_stream_stripped r]
        # g1 is touched first: it alone is a member, and it has read everything.
        r xack s g1 1-1
        assert_equal "db0_stream_distrib_streams_max_lag:0=1" [get_info_stream_field r stream_distrib_streams_max_lag]
        # Touching g2 enrols it, and the stream's sample becomes its lag of 6.
        r xgroup createconsumer s g2 c2
        assert_equal "db0_stream_distrib_streams_max_lag:4=1" [get_info_stream_field r stream_distrib_streams_max_lag]
        assert_equal [eval_stream_histogram r 0 stream_distrib_streams_max_lag max_lag] [get_info_stream_field r stream_distrib_streams_max_lag]
    }

    test "STREAM-STATS - lazy enable: a stream moved to another db while off is counted there" {
        # An in-place FLUSHDB gives db 0 a generation of its own. A MOVE while
        # stats are off carries db 0's stamps into db 1, and the re-enable must
        # not mistake them for db 1's: the moved group must register, and an
        # ack on it must not touch another group's sample.
        r config set stream-stats yes
        r FLUSHALL
        r multi
        r flushdb
        r exec
        seed_stream r s 2
        r xgroup create s g 0
        r xreadgroup group g c count 1 streams s >
        r config set stream-stats no
        r move s 1
        r config set stream-stats yes
        r select 1
        r xgroup setid s g 0
        assert_equal "db1_stream_distrib_streams_cgroups:1=1" [get_info_stream_field r stream_distrib_streams_cgroups]
        assert_equal "db1_stream_distrib_cgroups_pel:1=1" [get_info_stream_field r stream_distrib_cgroups_pel]
        assert_equal "db1_stream_distrib_cgroups_consumers:1=1" [get_info_stream_field r stream_distrib_cgroups_consumers]
        # The moved group's old heap belongs to db 0's generation and is gone;
        # SETID left its position unknown, so the lag row has no sample.
        assert_equal "" [get_info_stream_field r stream_distrib_streams_max_lag]
        seed_stream r t 1
        r xgroup create t g 0
        r xreadgroup group g c count 1 streams t >
        r xack s g 1-1
        assert_equal "db1_stream_distrib_cgroups_pel:0=1,1=1" [get_info_stream_field r stream_distrib_cgroups_pel]
        # t's group read its one entry: lag 0. s's is still unknown.
        assert_equal "db1_stream_distrib_streams_max_lag:0=1" [get_info_stream_field r stream_distrib_streams_max_lag]
        r select 0
    }

    test "STREAM-STATS - FLUSHDB of one database leaves another's counts intact" {
        r config set stream-stats yes
        r FLUSHALL
        seed_stream r A 4
        r xgroup create A g 0
        r xreadgroup group g c count 4 streams A >
        r select 5
        seed_stream r X 2
        r xgroup create X g 0
        r flushdb
        r select 0
        # The generation is per database: flushing db 5 must not un-count db 0.
        assert_equal "db0_stream_distrib_streams_cgroups:1=1" [get_info_stream_field r stream_distrib_streams_cgroups]
        assert_equal "db0_stream_distrib_cgroups_pel:4=1" [get_info_stream_field r stream_distrib_cgroups_pel]
        assert_equal "db0_stream_distrib_cgroups_consumers:1=1" [get_info_stream_field r stream_distrib_cgroups_consumers]
        assert_equal "db0_stream_distrib_streams_max_lag:0=1" [get_info_stream_field r stream_distrib_streams_max_lag]
        # ...and db 0's objects are still counted, so a change is an exact move.
        r xack A g 1-1 2-1
        assert_equal "db0_stream_distrib_cgroups_pel:2=1" [get_info_stream_field r stream_distrib_cgroups_pel]
    }

    test "STREAM-STATS - a failed multi-setting CONFIG SET leaves the histograms intact" {
        r config set stream-stats yes
        r FLUSHALL
        seed_stream r st 4
        r xgroup create st g 0
        r xreadgroup group g c count 4 streams st >
        assert_equal "db0_stream_distrib_cgroups_pel:4=1" [get_info_stream_field r stream_distrib_cgroups_pel]
        # Apply hooks run in argument order: `stream-stats no` is applied before
        # key-memory-histograms is rejected, and the rollback re-applies
        # `stream-stats yes`. Disabling leaves the rows alone and nothing can
        # run in between, so the re-enable finds them exact and keeps them.
        catch {r config set stream-stats no key-memory-histograms yes} err
        assert_match "*cannot be enabled at runtime*" $err
        assert_equal {yes} [lindex [r config get stream-stats] 1]
        assert_equal "db0_stream_distrib_streams_cgroups:1=1" [get_info_stream_field r stream_distrib_streams_cgroups]
        assert_equal "db0_stream_distrib_cgroups_pel:4=1" [get_info_stream_field r stream_distrib_cgroups_pel]
        assert_equal "db0_stream_distrib_cgroups_consumers:1=1" [get_info_stream_field r stream_distrib_cgroups_consumers]
        assert_equal "db0_stream_distrib_streams_max_lag:0=1" [get_info_stream_field r stream_distrib_streams_max_lag]
        # The generation survived too: the group is still counted, so PEL 4 -> 3
        # is an exact move into bin "2". A lost generation would re-enter the
        # sample instead and leave bin "4" behind (2=1,4=1).
        r xack st g 1-1
        assert_equal "db0_stream_distrib_cgroups_pel:2=1" [get_info_stream_field r stream_distrib_cgroups_pel]
        assert_equal [eval_stream_histogram r 0 stream_distrib_cgroups_pel pending] [get_info_stream_field r stream_distrib_cgroups_pel]
    }

    test "STREAM-STATS - a failed multi-setting CONFIG SET that enabled tracking leaves it off" {
        r config set stream-stats no
        r FLUSHALL
        seed_stream r st 4
        r xgroup create st g 0
        # The forward `yes` starts a new generation (streams changed while off),
        # the rollback turns tracking off again: hidden, and nothing counted.
        catch {r config set stream-stats yes key-memory-histograms yes} err
        assert_match "*cannot be enabled at runtime*" $err
        assert_equal {no} [lindex [r config get stream-stats] 1]
        assert_equal "" [get_info_stream_stripped r]
        # Enabling for real is lazy as usual: the group predates the generation
        # and is not counted until touched.
        r config set stream-stats yes
        assert_equal "" [get_info_stream_stripped r]
        r xreadgroup group g c count 1 streams st >
        assert_equal "db0_stream_distrib_cgroups_pel:1=1" [get_info_stream_field r stream_distrib_cgroups_pel]
    }

    test "STREAM-STATS - disabling and re-enabling with no stream change keeps the counts" {
        r config set stream-stats yes
        r FLUSHALL
        seed_stream r st 4
        r xgroup create st g 0
        r xreadgroup group g c count 4 streams st >
        r config set stream-stats no
        # Hidden while off; non-stream traffic does not disturb the rows.
        assert_equal "" [get_info_stream_stripped r]
        r set unrelated 1
        r config set stream-stats yes
        assert_equal "db0_stream_distrib_cgroups_pel:4=1" [get_info_stream_field r stream_distrib_cgroups_pel]
        assert_equal [eval_stream_histogram r 0 stream_distrib_cgroups_pel pending] [get_info_stream_field r stream_distrib_cgroups_pel]
        # The heap survived the cycle with the rows: the group is still a member.
        assert_equal "db0_stream_distrib_streams_max_lag:0=1" [get_info_stream_field r stream_distrib_streams_max_lag]
        assert_equal [eval_stream_histogram r 0 stream_distrib_streams_max_lag max_lag] [get_info_stream_field r stream_distrib_streams_max_lag]
        # A stream change while off does: enabling starts a new generation and
        # the group is counted again only once touched.
        r config set stream-stats no
        r xack st g 1-1
        r config set stream-stats yes
        assert_equal "" [get_info_stream_stripped r]
        r xack st g 2-1
        assert_equal "db0_stream_distrib_cgroups_pel:2=1" [get_info_stream_field r stream_distrib_cgroups_pel]
        assert_equal "db0_stream_distrib_streams_max_lag:0=1" [get_info_stream_field r stream_distrib_streams_max_lag]
    }

    test "STREAM-STATS - runtime enable is lazy, reload makes it exact" {
        # Start from stats off: the tests above may leave them enabled.
        r config set stream-stats no
        r FLUSHALL
        seed_stream r st 4
        r xgroup create st g 0
        r xreadgroup group g c count 4 streams st >

        # Enabling at runtime does not rescan: the group, created while tracking
        # was off, is not counted until a group command next touches it.
        r config set stream-stats yes
        assert_equal "" [get_info_stream_stripped r]

        # A reload rebuilds the gauges exactly from the keyspace.
        r DEBUG RELOAD
        assert_equal "db0_stream_distrib_cgroups_pel:4=1" [get_info_stream_field r stream_distrib_cgroups_pel]
        assert_equal "db0_stream_distrib_cgroups_consumers:1=1" [get_info_stream_field r stream_distrib_cgroups_consumers]
        assert_equal "db0_stream_distrib_streams_cgroups:1=1" [get_info_stream_field r stream_distrib_streams_cgroups]
        assert_equal "db0_stream_distrib_streams_max_lag:0=1" [get_info_stream_field r stream_distrib_streams_max_lag]

        # Disabling hides the section; the rows are kept for an exact re-enable
        # if nothing changes meanwhile.
        r config set stream-stats no
        assert_equal "" [get_info_stream_stripped r]
    }

    test "STREAM-STATS - XGROUP DESTROY while off frees the lag heap, the survivor re-enrols" {
        r config set stream-stats yes
        r FLUSHALL
        seed_stream r st 8
        r xgroup create st g1 0
        r xgroup create st g2 0
        r xreadgroup group g1 c count 8 streams st >
        assert_equal "db0_stream_distrib_streams_max_lag:0=1" [get_info_stream_field r stream_distrib_streams_max_lag]
        r xreadgroup group g2 c count 4 streams st >
        assert_equal "db0_stream_distrib_streams_max_lag:4=1" [get_info_stream_field r stream_distrib_streams_max_lag]

        # Disabling keeps the rows and the heap. A DESTROY while off sets
        # stream_stats_needs_reset, which condemns this generation, so it frees
        # the heap outright rather than taking g2 out of it; g1 is left with a
        # stale in-heap bit and no heap behind it.
        r config set stream-stats no
        assert_equal 1 [r xgroup destroy st g2]

        # Unarmed (see above), so the enable takes production's lazy path and
        # g1's stale bit is what the next commands meet.
        r config set stream-stats yes
        assert_equal "" [get_info_stream_stripped r]

        # XADD stamps the stream into the new generation and finds no heap to
        # free; the group is still uncounted.
        r xadd st 9-1 f v
        assert_equal "" [get_info_stream_field r stream_distrib_streams_max_lag]
        # The read refreshes g1's stamp, clearing the stale bit, and enrols it
        # in a fresh heap. It has read everything: lag 0.
        r xreadgroup group g1 c count 1 streams st >
        assert_equal "db0_stream_distrib_streams_max_lag:0=1" [get_info_stream_field r stream_distrib_streams_max_lag]
        r xadd st 10-1 f v
        assert_equal "db0_stream_distrib_streams_max_lag:1=1" [get_info_stream_field r stream_distrib_streams_max_lag]

        # A second member with a known, smaller lag: g3 reads all ten entries.
        # The row still shows g1, the worst.
        r xgroup create st g3 0
        r xreadgroup group g3 c count 10 streams st >
        assert_equal "db0_stream_distrib_streams_max_lag:1=1" [get_info_stream_field r stream_distrib_streams_max_lag]
        # With stats on, DESTROY takes g1 out of the live heap. g3 becomes the
        # sample with no further group command, so the heap was not wiped.
        assert_equal 1 [r xgroup destroy st g1]
        assert_equal "db0_stream_distrib_streams_max_lag:0=1" [get_info_stream_field r stream_distrib_streams_max_lag]
    }
}
