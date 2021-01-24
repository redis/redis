set testmodule [file normalize tests/modules/blockonbackground.so]

source tests/support/util.tcl

start_server {tags {"modules"}} {
    r module load $testmodule

    test { blocked clients time tracking - check blocked command that uses RedisModule_MeasureTimeStart() is tracking background time} {
        r slowlog reset
        r config set slowlog-log-slower-than 50000
        assert_equal [r slowlog len] 0
        r block.debug 0 10000
        assert_equal [r slowlog len] 0
        r config resetstat
        r block.debug 100 10000
        assert_equal [r slowlog len] 1

        set cmdstatline [cmdrstat block.debug r]

        regexp "calls=1,usec=(.*?),usec_per_call=(.*?),rejected_calls=0,failed_calls=0" $cmdstatline usec usec_per_call
        assert {$usec >= 100000}
        assert {$usec_per_call >= 100000}
    }

    test { blocked clients time tracking - check blocked command that uses RedisModule_MeasureTimeStart() is tracking background time even in timeout } {
        r slowlog reset
        r config set slowlog-log-slower-than 50000
        assert_equal [r slowlog len] 0
        r block.debug 0 10000
        assert_equal [r slowlog len] 0
        r config resetstat
        r block.debug 10000 100
        assert_equal [r slowlog len] 1
        # ensure only one key was populated

        set cmdstatline [cmdrstat block.debug r]

        regexp "calls=1,usec=(.*?),usec_per_call=(.*?),rejected_calls=0,failed_calls=0" $cmdstatline usec usec_per_call
        assert {$usec >= 100000}
        assert {$usec_per_call >= 100000}
    }

    test { blocked clients time tracking - check blocked command with multiple calls RedisModule_MeasureTimeStart()  is tracking the total background time } {
        r slowlog reset
        r config set slowlog-log-slower-than 50000
        assert_equal [r slowlog len] 0
        r block.double_debug 0
        assert_equal [r slowlog len] 0
        r config resetstat
        r block.double_debug 30
        assert_equal [r slowlog len] 1
        # ensure only one key was populated

        set cmdstatline [cmdrstat block.double_debug r]

        regexp "calls=1,usec=(.*?),usec_per_call=(.*?),rejected_calls=0,failed_calls=0" $cmdstatline usec usec_per_call
        assert {$usec >= 60000}
        assert {$usec_per_call >= 60000}
    }

    test { blocked clients time tracking - check blocked command without calling RedisModule_MeasureTimeStart() is not reporting background time } {
        r slowlog reset
        r config set slowlog-log-slower-than 5000
        assert_equal [r slowlog len] 0
        r block.debug_no_track 10 100
        # ensure slowlog is still empty
        assert_equal [r slowlog len] 0
        # ensure only one key was populated
        set cmdstatline [cmdrstat block.debug_no_track r]

        regexp "calls=1,usec=(.+),usec_per_call=(.+),rejected_calls=0,failed_calls=0" $cmdstatline usec usec_per_call
        assert_morethan 10000 $usec_per_call
    }
}
