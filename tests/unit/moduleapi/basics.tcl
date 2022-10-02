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
}

start_server {tags {"modules external:skip"} overrides {enable-module-command no}} {
    test {module command disabled} {
       assert_error "ERR *MODULE command not allowed*" {r module load $testmodule}
    }
}

set modules [list loadmodule $testmodule]
start_cluster 3 0 [list config_lines $modules] {
    set node1 [srv 0 client]
    set node2 [srv -1 client]
    set node3 [srv -2 client]

    test "Verify RM_Call inside module load function on cluster mode" {
        assert_equal {PONG} [$node1 PING]
        assert_equal {PONG} [$node2 PING]
        assert_equal {PONG} [$node3 PING]
    }
}