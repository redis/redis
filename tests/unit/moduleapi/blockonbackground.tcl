set testmodule [file normalize tests/modules/blockonbackground.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test {SLOWLOG - check that logs commands taking more time than specified including the background time} {
        r config set slowlog-log-slower-than 50000
        assert_equal [r slowlog len] 0
        r block.debug 0 10000
        assert_equal [r slowlog len] 0
        r config resetstat
        r block.debug 100 10000
        assert_equal [r slowlog len] 1
        # ensure only one key was populated

        set cmdstatline [cmdrstat block.debug r]

        regexp "calls=1,usec=(.*?),usec_per_call=(.*?),rejected_calls=0,failed_calls=0" $cmdstatline usec usec_per_call
        assert {$usec >= 100000}
        assert {$usec_per_call >= 100000}
    }
}
