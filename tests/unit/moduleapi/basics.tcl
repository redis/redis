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

    test "Unload the module - test" {
        assert_equal {OK} [r module unload test]
    }

    test "Unload the module in multi - test" {
        r module load $testmodule
        r multi
        r module unload test
        r test.basics
        assert_error {*Invalid command: test.basics*} {r exec}
    }

    test "Unload the module when module command already in multi queue - test" {
        set rd [redis_client]

        r module load $testmodule
        r multi
        r test.basics
        assert_equal {OK} [$rd module unload test]
        assert_error {*Invalid command: test.basics*} {r exec}
    }

    test "Unload the module when module command was referenced by client - test" {
        set rd [redis_client]
        r module load $testmodule
        r test.basics
        assert_equal {OK} [$rd module unload test]
        # Check test.basics command was removed from command list.
        assert_error {*ERR unknown command 'test.basics'*} {$rd test.basics}
        assert_match "*cmd=test.basics*" [$rd client list]
    }
}

start_server {tags {"modules external:skip"} overrides {enable-module-command no}} {
    test {module command disabled} {
       assert_error "ERR*MODULE command not allowed*" {r module load $testmodule}
    }
}