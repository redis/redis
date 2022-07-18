set testmodule [file normalize tests/modules/usercall.so]

set test_script "#!lua
redis.call('set','x',1)
return 1"

start_server {tags {"modules usercall"}} {
    r module load $testmodule

    test {test module check regular redis command without user/acl} {
        assert_equal [r usercall.call_without_acl set x 5] OK
    }

    test {test module check regular redis command with user/acl} {
        catch {r usercall.call_with_acl "~* &* +@all -set" set x 10} e
        assert_match {*NOPERM this user has no permissions to run the 'set' command*} $e
    }

    test {test module check eval script with user/acl} {
        set sha [r script load $test_script]
        assert_equal [r usercall.call_without_acl evalsha $sha 0] 1
    }

    test {test module check eval script without user/acl} {
        set sha [r script load $test_script]
        catch {r usercall.call_with_acl "~* &* +@all -set" evalsha $sha 0} e
        assert_match {*ERR The user executing the script can't run this command or subcommand script*} $e
    }
}