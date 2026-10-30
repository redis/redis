proc test_registration_errno {mode} {
    set testmodule [file normalize tests/modules/registration_errno.so]
    set seedmodule [file normalize tests/modules/datatype.so]

    test "Registration errno ($mode): load the existing datatype seed" {
        assert_equal 9 [string length "test___dt"]
        assert_equal OK [r module load $seedmodule]
    }

    test "Registration errno ($mode): a failed module can report EINVAL" {
        assert_error {*ERR Error loading the extension*} {r module load $testmodule abort}
        wait_for_log_messages 0 {"*registration errno abort: EINVAL*"} 0 10 10
    }

    test "Registration errno ($mode): normal module loading still succeeds" {
        assert_equal OK [r module load $testmodule]
        assert_equal OK [r registrationerrno.valid]
        assert_equal OK [r registrationerrno.nullflags]
    }

    set cases [r registrationerrno.results]
    test "Registration errno ($mode): all load-time cases were recorded" {
        assert_equal 20 [llength $cases]
    }
    foreach row $cases {
        lassign $row label actual_success expected_success actual_errno expected_errno
        test "Registration errno ($mode): $label" {
            assert_equal $actual_success $expected_success
            if {!$expected_success} {
                assert_equal $actual_errno $expected_errno
            }
        }
    }

    set cases [r registrationerrno.outside]
    test "Registration errno ($mode): both outside-load cases were recorded" {
        assert_equal 2 [llength $cases]
    }
    foreach row $cases {
        lassign $row label actual_success expected_success actual_errno expected_errno
        test "Registration errno ($mode): $label" {
            assert_equal $actual_success $expected_success
            assert_equal $actual_errno $expected_errno
        }
    }
}

start_server {tags {"modules external:skip"}} {
    test_registration_errno standalone
}

start_cluster 1 0 {tags {modules external:skip cluster}} {
    test_registration_errno cluster
}
