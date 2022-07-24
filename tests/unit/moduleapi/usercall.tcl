set testmodule [file normalize tests/modules/usercall.so]

set test_script_set "#!lua
redis.call('set','x',1)
return 1"

set test_script_get "#!lua
redis.call('get','x')
return 1"

start_server {tags {"modules usercall"}} {
    r module load $testmodule

    test {test module check regular redis command without user/acl} {
        assert_equal [r usercall.call_without_acl set x 5] OK
    }

    test {test module check regular redis command with user/acl} {
        assert_equal [r set x 5] OK

        assert_equal [r usercall.reset_user] OK
        assert_equal [r usercall.add_to_acl "~* &* +@all -set"] OK

        catch {r usercall.call_with_acl set x 10} e
        assert_match {*NOPERM this user has no permissions to run the 'set' command*} $e

        assert_equal [r usercall.call_with_acl get x] 5
    }

    test {test module check eval script with user/acl} {
        set sha_set [r script load $test_script_set]
        set sha_get [r script load $test_script_get]

        assert_equal [r usercall.call_without_acl evalsha $sha_set 0] 1
        assert_equal [r usercall.call_without_acl evalsha $sha_get 0] 1
    }

    test {test module check eval script without user/acl} {
        set sha_set [r script load $test_script_set]
        set sha_get [r script load $test_script_get]

        assert_equal [r usercall.reset_user] OK
        assert_equal [r usercall.add_to_acl "~* &* +@all -set"] OK

        catch {r usercall.call_with_acl evalsha $sha_set 0} e
        assert_match {*ERR The user executing the script can't run this command or subcommand script*} $e

        assert_equal [r usercall.call_with_acl evalsha $sha_get 0] 1
    }
}