
source "../tests/includes/init-tests.tcl"

set ::user "testuser"
set ::password "secret"

proc setup_acl {} {
    foreach_sentinel_id id {
        assert_equal {OK} [S $id ACL SETUSER $::user >$::password +@all on]
        assert_equal {OK} [S $id ACL SETUSER default off]

        S $id CLIENT KILL USER default SKIPME no
        assert_equal {OK} [S $id AUTH $::user $::password]
    }
}

proc teardown_acl {} {
    foreach_sentinel_id id {
        assert_equal {OK} [S $id ACL SETUSER default on]
        assert_equal {1} [S $id ACL DELUSER $::user]

        S $id SENTINEL CONFIG SET sentinel-user ""
        S $id SENTINEL CONFIG SET sentinel-pass ""
    }
}

test "(post-init) Set up ACL configuration" {
    setup_acl
    assert_equal $::user [S 1 ACL WHOAMI]
}

test "SENTINEL CONFIG SET handles on-the-fly credentials reconfiguration" {
    # Make sure we're starting with a broken state...
    wait_for_condition 200 50 {
        [catch {S 1 SENTINEL CKQUORUM mymaster}] == 1
    } else {
        fail "Expected: Sentinel to be disconnected from master due to wrong password"
    }
    assert_error "*NOQUORUM*" {S 1 SENTINEL CKQUORUM mymaster}

    foreach_sentinel_id id {
        assert_equal {OK} [S $id SENTINEL CONFIG SET sentinel-user $::user]
        assert_equal {OK} [S $id SENTINEL CONFIG SET sentinel-pass $::password]
    }

    wait_for_condition 200 50 {
        [catch {S 1 SENTINEL CKQUORUM mymaster}] == 0
    } else {
         fail "Expected: Sentinel to be connected to master after setting password"
    }
    assert_match {*OK*} [S 1 SENTINEL CKQUORUM mymaster]
}

test "(post-cleanup) Tear down ACL configuration" {
    teardown_acl
}
