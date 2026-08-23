set testmodule [file normalize tests/modules/aofduration.so]

proc RedisModule_run_steps { server args } {
    set cmd  "aofd.rm_run_steps"
    foreach arg $args {
        set cmd [concat $cmd [llength $arg] $arg]
    }
    $server {*}$cmd
}

proc RedisModule_run_steps_bg { server args } {
    set cmd  "aofd.rm_run_steps_bg"
    foreach arg $args {
        set cmd [concat $cmd [llength $arg] $arg]
    }
    $server {*}$cmd
}

# Reset AOF duration for each test step
proc reset_aof_duration {} {
    r config set appendonly no
    r config set appendonly yes
    r config set auto-aof-rewrite-percentage 0 ; # Disable auto-rewrite.
    waitForBgrewriteaof r
}

# Run 'setup' then 'cmd' 'iterations' times, summing how much 'cmd' alone
# adds to aof_cmd_duration each time (reset_aof_duration isolates it from
# 'setup'). Also returns the real total time 'cmd' took, per `INFO
# commandstats` (cmd->microseconds - the exact same per-call value that feeds
# the aof_cmd_duration pot). Comparing the two exposes double-counting while
# cancelling out unrelated timing noise, since both read the same timer.
proc sum_duration_delta_vs_real {cmdname iterations setup cmd} {
    r config resetstat
    set delta_sum 0
    for {set i 0} {$i < $iterations} {incr i} {
        uplevel 1 $setup
        reset_aof_duration
        set d_before [s aof_cmd_duration]
        uplevel 1 $cmd
        set d_after [s aof_cmd_duration]
        incr delta_sum [expr {$d_after - $d_before}]
    }
    set real_usec 0
    foreach line [split [r info commandstats] "\r\n"] {
        if {[string match "cmdstat_$cmdname:*" $line]} {
            regexp {usec=(\d+)} $line -> real_usec
        }
    }
    list $delta_sum $real_usec
}

start_server [list tags {"modules external:skip"} overrides [list loadmodule "$testmodule"]] {

    test { AOF Duration - duration of module command counted by aof_cmd_duration } {

        # simulated delay is considerably bigger than invoked operations
        set delayusec 50000

        # Test:     Run 'set' command and verify aof_cmd_duration advanced
        # Expected: Only set command will be measured by AOF duration ( 0 < d  < delayusec)
        reset_aof_duration
        r set x 1
        r set x 1
        r set x 1
        set d [s aof_cmd_duration]
        assert_range_exclude $d 0 $delayusec

        # Test:     rm_call 'set' with delayusec (without '!')
        # Expected: None will be measured by AOF duration
        reset_aof_duration
        RedisModule_run_steps r \
            [list "rm_call" "set" "x" "1"] \
            [list "sleep_usec" $delayusec]
        set d [s aof_cmd_duration]
        assert_equal $d 0

        # Test:     rm_call! 'set' with delayusec
        # Expected: Only 'set' will be measured  ( 0 < d  < delayusec)
        reset_aof_duration
        RedisModule_run_steps r \
            [list "rm_call_flags" "!" "set" "x" "1"] \
            [list "rm_call_flags" "!" "set" "x" "1"] \
            [list "rm_call_flags" "!" "set" "x" "1"] \
            [list "sleep_usec" $delayusec]
        set d [s aof_cmd_duration]
        assert_range_exclude $d 0 $delayusec

        # Test:     'get x' in module command, with delayusec
        # Expected: None will be measured by AOF duration
        reset_aof_duration
        RedisModule_run_steps r \
            [list "rm_call_flags" "!" "get" "x"] \
            [list "sleep_usec" $delayusec]
        set d [s aof_cmd_duration]
        assert_equal $d 0

        # Test:     eval 'set x' and delayusec
        # Expected: Only 'set' command will be measured by AOF duration
        reset_aof_duration
        RedisModule_run_steps r \
            "rm_call_flags ! eval {redis.call('set','x',1) return 1} 1 x" \
            "rm_call_flags ! eval {redis.call('set','x',1) return 1} 1 x" \
            "rm_call_flags ! eval {redis.call('set','x',1) return 1} 1 x" \
            [list "sleep_usec" $delayusec]
        set d [s aof_cmd_duration]
        assert_range_exclude $d 0 $delayusec

        # Test:     Replicate in module command, and add delayusec after
        # Expected: Total time to run module command will be measured by AOF duration
        reset_aof_duration
        RedisModule_run_steps r \
            [list "rm_replicate" "set" "x" "1"] \
            [list "rm_replicate" "set" "x" "1"] \
            [list "rm_replicate" "set" "x" "1"] \
            [list "sleep_usec" $delayusec]
        set d [s aof_cmd_duration]
        assert_morethan_equal $d $delayusec

        # Test:     ReplicateVerbatim in module command, and add delayusec after
        # Expected: Total time to run module command will be measured by AOF duration
        reset_aof_duration
        RedisModule_run_steps r \
            [list "rm_replicateVerbatim"] \
            [list "sleep_usec" $delayusec]
        set d [s aof_cmd_duration]
        assert_morethan_equal $d $delayusec
    }

    test { AOF Duration - blocked clients time tracking flows } {
        set delayusec 50000

        # Test:     Blocked client wait delayusec and calls RM_Replicate
        # Expected: BlockedClientMeasureTimeStart/End is counted by AOF duration
        reset_aof_duration
        RedisModule_run_steps_bg r \
            [list "rm_replicate" "set" "x" "1"] \
            [list "rm_replicate" "set" "x" "1"] \
            [list "rm_replicate" "set" "x" "1"] \
            [list "sleep_usec" $delayusec]
        set d [s aof_cmd_duration]
        assert_morethan_equal $d $delayusec

        # Test:     Blocked client wait delayusec and calls RM_ReplicateVerbatim with
        # Expected: BlockedClientMeasureTimeStart/End is counted by AOF duration
        reset_aof_duration
        RedisModule_run_steps_bg r \
            [list "rm_replicateVerbatim"] \
            [list "sleep_usec" $delayusec]
        set d [s aof_cmd_duration]
        assert_morethan_equal $d $delayusec

        # Test:     Blocked client wait delayusec (without calling RM_Replicate*)
        # Expected: BlockedClientMeasureTimeStart/End won't be counted by AOF duration
        reset_aof_duration
        RedisModule_run_steps_bg r \
            [list "sleep_usec" $delayusec]
        set d [s aof_cmd_duration]
        assert_equal $d 0

        # Test:     Blocked client wait delayusec and then RM_Call (without '!')
        # Expected: None will be counted by AOF duration
        reset_aof_duration
        RedisModule_run_steps_bg r \
            [list "sleep_usec" $delayusec] \
            [list "rm_call" "set" "x" "1"]
        set d [s aof_cmd_duration]
        assert_equal $d 0

        # Test:     Blocked client wait delayusec and get x' in module command
        # Expected: None will be measured by AOF duration
        reset_aof_duration
        RedisModule_run_steps_bg r \
            [list "rm_call_flags" "!" "get" "x"] \
            [list "sleep_usec" $delayusec]
        set d [s aof_cmd_duration]
        assert_equal $d 0

        # Test:     Blocked client wait delayusec and then RM_Call!
        # Expected: BlockedClientMeasureTimeStart/End won't be counted by AOF duration,
        #           only offstrings commands of RM_Call!
        reset_aof_duration
        RedisModule_run_steps_bg r \
            [list "sleep_usec" $delayusec] \
            [list "rm_call_flags" "!" "set" "x" "1"] \
            [list "rm_call_flags" "!" "set" "x" "1"] \
            [list "rm_call_flags" "!" "set" "x" "1"]
        set d [s aof_cmd_duration]
        assert_range_exclude $d 0 $delayusec
    }

    test { AOF Duration - fix: SPOP with count rewrites propagation to SREM but its execution time is now counted } {
        # SPOP with a count does real work (picking random members) but then calls
        # preventCommandPropagation() and manually propagates the rewrite as SREM
        # commands via alsoPropagate(). That helper used to hard-code duration=0,
        # so none of that real work was counted by aof_cmd_duration even though
        # the rewritten SREM commands were genuinely written to the AOF.
        #
        # Fixed by propagating with PROP_DURATION_UNKNOWN instead, so the
        # real, measured duration of the enclosing SPOP call is credited instead
        # of being silently dropped.
        r del myset
        r sadd myset a b c d e f g h i j
        reset_aof_duration

        set size_before [s aof_current_size]
        set d_before [s aof_cmd_duration]

        r spop myset 5

        set size_after [s aof_current_size]
        set d_after [s aof_cmd_duration]

        # The SPOP was indeed propagated to the AOF (as a batch of SREMs)...
        assert_morethan $size_after $size_before
        # ...and now its execution time IS added to aof_cmd_duration.
        assert_morethan $d_after $d_before

        # For contrast: an equivalent, directly-issued SREM (the same rewritten
        # form SPOP produces under the hood) also gets its duration counted,
        # since it goes through the normal auto-propagation path in call().
        r sadd myset a b c d e
        set d_before2 [s aof_cmd_duration]
        r srem myset a b c d e
        set d_after2 [s aof_cmd_duration]
        assert_morethan $d_after2 $d_before2
    }

    test { AOF Duration - fix: HEXPIRE with a past TTL rewrites propagation to HDEL but its execution time is now counted } {
        # HEXPIRE (and friends) with a TTL already in the past deletes the
        # field(s) synchronously via propagateHashFieldDeletion(), which calls
        # preventCommandPropagation() on the HEXPIRE itself and manually
        # propagates the field deletion(s) as HDEL. Same class of bug as SPOP.
        r del myhash
        r hset myhash f1 v1 f2 v2 f3 v3
        reset_aof_duration

        set size_before [s aof_current_size]
        set d_before [s aof_cmd_duration]

        r hexpireat myhash 1 FIELDS 1 f1

        set size_after [s aof_current_size]
        set d_after [s aof_cmd_duration]

        # The HEXPIRE was propagated to the AOF (as an HDEL)...
        assert_morethan $size_after $size_before
        # ...and now its execution time IS added to aof_cmd_duration.
        assert_morethan $d_after $d_before
    }

    test { AOF Duration - fix: HGETEX with a stale expired field plus a fresh field does not double-count duration } {
        # HGETEX can, in a single call, lazily reap a field whose *pre-existing*
        # expiry has already passed while reading it (queuing an HDEL via
        # propagateHashFieldDeletion(), tagged PROP_DURATION_UNKNOWN) while ALSO
        # successfully setting a *new* expiry on a different field in that very
        # same call (auto-propagated as its own canonical HPEXPIREAT, with a
        # known, already-measured duration). Before the fix, both of these
        # independently claimed the same measured call() duration, so
        # aof_cmd_duration was credited twice for one call.
        #
        # A single command's duration is only a few microseconds and noisy on
        # its own, so run several iterations and compare summed deltas vs. the
        # real time reported by `INFO commandstats` - this smooths out the
        # noise while still clearly exposing the ~2x effect of the bug.

        # Disable active expire so f_old's short TTL can only ever be
        # discovered lazily - by the HGETEX call itself - and never reaped
        # early by the background cron, which would make the repro flaky.
        r debug set-active-expire 0
        lassign [sum_duration_delta_vs_real hgetex 10 {
            r del myhash
            r hset myhash f_old v1 f_fresh v2
            r hpexpire myhash 1 FIELDS 1 f_old
            after 20 ; # let f_old's short TTL actually pass without touching the key again
        } {
            r hgetex myhash EX 100000 FIELDS 2 f_old f_fresh
        }] mixed_delta_sum real_usec
        r debug set-active-expire 1

        assert_morethan $mixed_delta_sum 0
        assert_morethan $real_usec 0
        # Before the fix, mixed_delta_sum was roughly double real_usec (both
        # the HDEL and the HPEXPIREAT independently credited the same call()'s
        # real duration). After the fix, it should match real_usec closely -
        # clearly under double.
        assert_lessthan $mixed_delta_sum [expr {$real_usec * 16 / 10}]
    }

    test { AOF Duration - fix: HGETDEL with a stale expired field plus a fresh field does not double-count duration } {
        # HGETDEL can, in a single call, lazily reap a field whose
        # *pre-existing* expiry has already passed while reading it (queuing
        # an HDEL via propagateHashFieldDeletion(), tagged
        # PROP_DURATION_UNKNOWN) while ALSO explicitly deleting a different,
        # still-valid field in that very same call (rewriting itself to a
        # canonical HDEL, auto-propagated with its own known, already-measured
        # duration). Same class of bug as HGETEX.
        r debug set-active-expire 0
        lassign [sum_duration_delta_vs_real hgetdel 10 {
            r del myhash
            r hset myhash f_old v1 f_fresh v2
            r hpexpire myhash 1 FIELDS 1 f_old
            after 20 ; # let f_old's short TTL actually pass without touching the key again
        } {
            r hgetdel myhash FIELDS 2 f_old f_fresh
        }] mixed_delta_sum real_usec
        r debug set-active-expire 1

        assert_morethan $mixed_delta_sum 0
        assert_morethan $real_usec 0
        assert_lessthan $mixed_delta_sum [expr {$real_usec * 16 / 10}]
    }

    test { AOF Duration - fix: XCLAIM rewrites its own propagation but its execution time is now counted } {
        # XCLAIM does real work (scanning/updating the consumer group PEL) but
        # calls preventCommandPropagation() and manually propagates an
        # idempotent XCLAIM via streamPropagateXCLAIM(). Same class of bug.
        r del mystream
        set id [r xadd mystream * field value]
        r xgroup create mystream grp 0
        r xreadgroup GROUP grp consumer1 COUNT 1 STREAMS mystream >
        reset_aof_duration

        set size_before [s aof_current_size]
        set d_before [s aof_cmd_duration]

        r xclaim mystream grp consumer2 0 $id

        set size_after [s aof_current_size]
        set d_after [s aof_cmd_duration]

        # The XCLAIM was propagated to the AOF (as an idempotent XCLAIM)...
        assert_morethan $size_after $size_before
        # ...and now its execution time IS added to aof_cmd_duration.
        assert_morethan $d_after $d_before
    }

    test { AOF Duration - propagation without duration is not counted while AOF is disabled } {
        # Ops marked with PROP_DURATION_UNKNOWN (SPOP w/ count, HEXPIRE, XCLAIM,
        # or any module using RM_Replicate/RM_ReplicateVerbatim) only learn their
        # real duration once the whole call() finishes. That real duration must
        # only be credited to aof_cmd_duration if the op is actually being
        # written to the AOF - not just because it happened to also be sent to
        # a replica while AOF was off.
        r config set appendonly no
        r del myset
        r sadd myset a b c d e f g h i j

        start_server {} {
            r slaveof [srv -1 host] [srv -1 port]
            wait_for_sync r

            # AOF is off on the primary; this SPOP is only propagated to the
            # replica below, via a PROP_DURATION_UNKNOWN-marked SREM.
            assert_equal [s -1 aof_enabled] 0
            r -1 spop myset 5

            # Enabling AOF now must not show a bogus leftover duration that
            # was measured while AOF was off and had nothing to do with AOF.
            r -1 config set appendonly yes
            assert_equal [s -1 aof_cmd_duration] 0

            waitForBgrewriteaof [srv -1 client]
            r -1 config set appendonly no
        }
    }

    test { AOF Duration - EVAL SET+SPOP counts inner calls not Lua time } {
        # Sleep inside Lua is not an AOF write. Only SET and SPOP should
        # move aof_cmd_duration; leftover must not dump the sleep onto SREM.
        r del k s
        r sadd s a b c d e
        reset_aof_duration
        r eval {redis.call('aofd.sleep_usec','50000'); redis.call('SET','k','1'); redis.call('SPOP','s',2); return 1} 2 k s
        set d [s aof_cmd_duration]
        assert_range_exclude $d 0 50000
    }

    test { AOF Duration - blocked RM_Replicate+sleep is counted once not twice } {
        set delayusec 50000
        reset_aof_duration
        RedisModule_run_steps_bg r \
            [list "rm_replicate" "set" "x" "1"] \
            [list "sleep_usec" $delayusec]
        set d [s aof_cmd_duration]
        assert_morethan_equal $d $delayusec
        assert_lessthan $d [expr {$delayusec * 2}]
    }
}
