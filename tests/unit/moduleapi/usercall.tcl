set testmodule [file normalize tests/modules/usercall.so]

set test_script_set "#!lua
redis.call('set','x',1)
return 1"

set test_script_get "#!lua
redis.call('get','x')
return 1"

start_server {tags {"modules usercall"}} {
    r module load $testmodule

    # baseline test that module isn't doing anything weird
    test {test module check regular redis command without user/acl} {
        assert_equal [r usercall.reset_user] OK
        assert_equal [r usercall.add_to_acl "~* &* +@all -set"] OK
        assert_equal [r usercall.call_without_user set x 5] OK
        assert_equal [r usercall.reset_user] OK
    }

    # call with user with acl set on it, but without testing the acl
    test {test module check regular redis command with user} {
        assert_equal [r set x 5] OK

        assert_equal [r usercall.reset_user] OK
        assert_equal [r usercall.add_to_acl "~* &* +@all -set"] OK
        # off and sanitize-payload because module user / default value
        assert_equal [r usercall.get_acl] "off sanitize-payload ~* &* +@all -set"

        # doesn't fail for regular commands as just testing acl here
        assert_equal [r usercall.call_with_user_flag {} set x 10] OK

        assert_equal [r get x] 10
        assert_equal [r usercall.reset_user] OK
    }

    # call with user with acl set on it, but with testing the acl in rm_call (for cmd itself)
    test {test module check regular redis command with user and acl} {
        assert_equal [r set x 5] OK

        r ACL LOG RESET
        assert_equal [r usercall.reset_user] OK
        assert_equal [r usercall.add_to_acl "~* &* +@all -set"] OK
        # off and sanitize-payload because module user / default value
        assert_equal [r usercall.get_acl] "off sanitize-payload ~* &* +@all -set"

        # fails here as testing acl in rm call
        assert_error {*NOPERM User module_user has no permissions*} {r usercall.call_with_user_flag C set x 10}

        assert_equal [r usercall.call_with_user_flag C get x] 5

        # verify that new log entry added
        set entry [lindex [r ACL LOG] 0]
        assert_equal [dict get $entry username] {module_user}
        assert_equal [dict get $entry context] {module}
        assert_equal [dict get $entry object] {set}
        assert_equal [dict get $entry reason] {command}
        assert_match {*cmd=usercall.call_with_user_flag*} [dict get $entry client-info]

        assert_equal [r usercall.reset_user] OK
    }

    # call with user with acl set on it, but with testing the acl in rm_call (for cmd itself)
    test {test module check regular redis command with user and acl from blocked background thread} {
        assert_equal [r set x 5] OK

        r ACL LOG RESET
        assert_equal [r usercall.reset_user] OK
        assert_equal [r usercall.add_to_acl "~* &* +@all -set"] OK

        # fails here as testing acl in rm call from a background thread
        assert_error {*NOPERM User module_user has no permissions*} {r usercall.call_with_user_bg C set x 10}

        assert_equal [r usercall.call_with_user_bg C get x] 5

        # verify that new log entry added
        set entry [lindex [r ACL LOG] 0]
        assert_equal [dict get $entry username] {module_user}
        assert_equal [dict get $entry context] {module}
        assert_equal [dict get $entry object] {set}
        assert_equal [dict get $entry reason] {command}
        assert_match {*cmd=NULL*} [dict get $entry client-info]

        assert_equal [r usercall.reset_user] OK
    }

    # baseline script test, call without user on script
    test {test module check eval script without user} {
        set sha_set [r script load $test_script_set]
        set sha_get [r script load $test_script_get]

        assert_equal [r usercall.call_without_user evalsha $sha_set 0] 1
        assert_equal [r usercall.call_without_user evalsha $sha_get 0] 1
    }

    # baseline script test, call without user on script
    test {test module check eval script with user being set, but not acl testing} {
        set sha_set [r script load $test_script_set]
        set sha_get [r script load $test_script_get]

        assert_equal [r usercall.reset_user] OK
        assert_equal [r usercall.add_to_acl "~* &* +@all -set"] OK
        # off and sanitize-payload because module user / default value
        assert_equal [r usercall.get_acl] "off sanitize-payload ~* &* +@all -set"

        # passes as not checking ACL
        assert_equal [r usercall.call_with_user_flag {} evalsha $sha_set 0] 1
        assert_equal [r usercall.call_with_user_flag {} evalsha $sha_get 0] 1
    }

    # call with user on script (without rm_call acl check) to ensure user carries through to script execution
    # we already tested the check in rm_call above, here we are checking the script itself will enforce ACL
    test {test module check eval script with user and acl} {
        set sha_set [r script load $test_script_set]
        set sha_get [r script load $test_script_get]

        r ACL LOG RESET
        assert_equal [r usercall.reset_user] OK
        assert_equal [r usercall.add_to_acl "~* &* +@all -set"] OK

        # fails here in script, as rm_call will permit the eval call
        catch {r usercall.call_with_user_flag C evalsha $sha_set 0} e
        assert_match {*ERR ACL failure in script*} $e

        assert_equal [r usercall.call_with_user_flag C evalsha $sha_get 0] 1

        # verify that new log entry added
        set entry [lindex [r ACL LOG] 0]
        assert_equal [dict get $entry username] {module_user}
        assert_equal [dict get $entry context] {lua}
        assert_equal [dict get $entry object] {set}
        assert_equal [dict get $entry reason] {command}
        assert_match {*cmd=usercall.call_with_user_flag*} [dict get $entry client-info]
    }

    test {server not crashing when MONITOR is fed from spawned thread} {
        set rd [redis_deferring_client]
        $rd monitor

        r ACL LOG RESET
        assert_equal [r usercall.reset_user] OK
        assert_equal [r usercall.add_to_acl "~* &* +@all -set"] OK

        r flushdb
        r set x x

        # This is enough. This checks that we don't crash inside
        # updateClientMemUsageAndBucket
        assert_equal x [r usercall.call_with_user_bg C get x]

        $rd close
    }

    start_server {tags {"wait aof network external:skip"}} {
        set slave [srv 0 client]
        set slave_host [srv 0 host]
        set slave_port [srv 0 port]
        set slave_pid [srv 0 pid]
        set master [srv -1 client]
        set master_host [srv -1 host]
        set master_port [srv -1 port]

        $master config set appendonly yes
        $master config set appendfsync everysec
        $slave config set appendonly yes
        $slave config set appendfsync everysec

        test {Setup slave} {
            $slave slaveof $master_host $master_port
            wait_for_condition 50 100 {
                [s 0 master_link_status] eq {up}
            } else {
                fail "Replication not started."
            }
        }

        test {test module replicate only to replicas and WAITAOF} {
            $master set x 1
            assert_equal [$master waitaof 1 1 10000] {1 1}
            $master usercall.call_with_user_flag A! config set loglevel notice
            # Make sure WAITAOF doesn't hang
            assert_equal [$master waitaof 1 1 10000] {1 1}
        }
    }
}
