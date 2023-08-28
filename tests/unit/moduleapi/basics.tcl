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
        foreach resp {3 2} {
            if {[lsearch $::denytags "resp3"] >= 0} {
                if {$resp == 3} {continue}
            } elseif {$::force_resp3} {
                if {$resp == 2} {continue}
            }
            r hello $resp
            set reply [r test.getresp]
            assert_equal $reply $resp
            r hello 2
        }
    }

    test "Unload the module - test" {
        assert_equal {OK} [r module unload test]
    }
}

start_server {tags {"modules external:skip"} overrides {enable-module-command no}} {
    test {module command disabled} {
       assert_error "ERR *MODULE command not allowed*" {r module load $testmodule}
    }
}
