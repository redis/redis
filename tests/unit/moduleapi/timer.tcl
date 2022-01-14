set testmodule [file normalize tests/modules/timer.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test {RM_CreateTimer: a sequence of timers work} {
        # We can't guarantee same-ms but we try using MULTI/EXEC
        r multi
        for {set i 0} {$i < 20} {incr i} {
            r test.createtimer 10 timer-incr-key
        }
        r exec

        after 500
        assert_equal 20 [r get timer-incr-key]
    }

    test {RM_GetTimer: basic sanity} {
        # Getting non-existing timer
        assert_equal {} [r test.gettimer 0]

        # Getting a real timer
        set id [r test.createtimer 10000 timer-incr-key]
        set info [r test.gettimer $id]

        assert_equal "timer-incr-key" [lindex $info 0]
        set remaining [lindex $info 1]
        assert {$remaining < 10000 && $remaining > 1}
    }

    test {RM_StopTimer: basic sanity} {
        r set "timer-incr-key" 0
        set id [r test.createtimer 1000 timer-incr-key]

        assert_equal 1 [r test.stoptimer $id]

        # Wait to be sure timer doesn't execute
        after 2000
        assert_equal 0 [r get timer-incr-key]

        # Stop non-existing timer
        assert_equal 0 [r test.stoptimer $id]
    }

    test {Timer appears non-existing after it fires} {
        r set "timer-incr-key" 0
        set id [r test.createtimer 10 timer-incr-key]

        # verify timer fired
        after 500
        assert_equal 1 [r get timer-incr-key]

        # verify id does not exist
        assert_equal {} [r test.gettimer $id]
    }

    test {Busy module} {
        set script_time_limit 50
        set old_time_limit [lindex [r config get script-time-limit] 1]
        r config set script-time-limit $script_time_limit

        # run blocking command
        set rd [redis_deferring_client]
        set start [clock clicks -milliseconds]
        $rd test.busy_module
        $rd flush

        # make sure we get BUSY error, and that we didn't get it too early
        assert_error {*BUSY Slow module operation*} {r ping}
        assert_morethan [expr [clock clicks -milliseconds]-$start] $script_time_limit

        # abort the blocking operation
        r test.stop_busy_module
        wait_for_condition 50 100 {
            [r ping] eq {PONG}
        } else {
            fail "Failed waiting for busy command to end"
        }
        $rd read
        $rd close
        r config set script-time-limit $old_time_limit
    }
}

