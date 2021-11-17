set testmodule [file normalize tests/modules/basics.so]


start_server {tags {"modules"}} {
    r module load $testmodule

    test {test module api basics} {
        r test.basics
    } {ALL TESTS PASSED}

    test {test rm_call auto mode} {
        r hello 2
        set reply [r test.rmcallautomode]
        assert_equal [lindex $reply 0] f1
        assert_equal [lindex $reply 1] v1
        assert_equal [lindex $reply 2] f2
        assert_equal [lindex $reply 3] v2
        r hello 3
        set reply [r test.rmcallautomode]
        assert_equal [dict get $reply f1] v1
        assert_equal [dict get $reply f2] v2
    }

    test {test get resp} {
        r hello 2
        set reply [r test.getresp]
        assert_equal $reply 2
        r hello 3
        set reply [r test.getresp]
        assert_equal $reply 3
    }

    test {Unsafe command names are sanitized in INFO output} {
        r test.weird:cmd
        set info [r info commandstats]
        assert_match {*cmdstat_test.weird_cmd:calls=1*} $info
    }

    r module unload test
}
