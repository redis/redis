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
}

