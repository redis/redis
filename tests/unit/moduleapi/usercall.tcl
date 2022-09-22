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
        assert_equal [r usercall.get_acl] "off ~* &* +@all -set"

        # doesn't fail for regular commands as just testing acl here
        assert_equal [r usercall.call_with_user_flag {} set x 10] OK

        assert_equal [r get x] 10
        assert_equal [r usercall.reset_user] OK
    }

    # call with user with acl set on it, but with testing the acl in rm_call (for cmd itself)
    test {test module check regular redis command with user and acl} {
        assert_equal [r set x 5] OK

        assert_equal [r usercall.reset_user] OK
        assert_equal [r usercall.add_to_acl "~* &* +@all -set"] OK
        # off and sanitize-payload because module user / default value
        assert_equal [r usercall.get_acl] "off ~* &* +@all -set"

        # fails here as testing acl in rm call
        catch {r usercall.call_with_user_flag C set x 10} e
        assert_match {*ERR acl verification failed*} $e

        assert_equal [r usercall.call_with_user_flag C get x] 5

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
        assert_equal [r usercall.get_acl] "off ~* &* +@all -set"

        # passes as not checking ACL
        assert_equal [r usercall.call_with_user_flag {} evalsha $sha_set 0] 1
        assert_equal [r usercall.call_with_user_flag {} evalsha $sha_get 0] 1
    }

    # call with user on script (without rm_call acl check) to ensure user carries through to script execution
    # we already tested the check in rm_call above, here we are checking the script itself will enforce ACL
    test {test module check eval script with user and acl} {
        set sha_set [r script load $test_script_set]
        set sha_get [r script load $test_script_get]

        assert_equal [r usercall.reset_user] OK
        assert_equal [r usercall.add_to_acl "~* &* +@all -set"] OK

        # fails here in script, as rm_call will permit the eval call
        catch {r usercall.call_with_user_flag C evalsha $sha_set 0} e
        assert_match {*ERR The user executing the script can't run this command or subcommand script*} $e

        assert_equal [r usercall.call_with_user_flag C evalsha $sha_get 0] 1
    }
}