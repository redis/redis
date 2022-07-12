set testmodule [file normalize tests/modules/usercall.so]

start_server {tags {"modules usercall"}} {
    r module load $testmodule

    test {test module check regular redis command without user/acl} {
        assert_equal [r usercall.call_without_acl set x 5] OK
    }

    test {test module check regular redis command with user/acl} {
        catch {r usercall.call_with_acl -set set x 10} e
        assert_match {*NOPERM this user has no permissions to run the 'set' command*} $e
    }

    test {test module check eval script with user/acl} {

    }

    test {test module check eval script without user/acl} {

    }
}