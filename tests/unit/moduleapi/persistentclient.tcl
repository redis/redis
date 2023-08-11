set testmodule [file normalize tests/modules/persistentclient.so]

start_server {tags {"modules persistentclient"}} {
    r module load $testmodule

    test {test basic persistent client exec} {
        r del x
        r mc.delete
        r mc.create
        r set x 1
        assert_equal [r mc.exec get x] 1
        r mc.delete
    }

    test {test persistent client watch} {
        r del x
        r mc.delete
        r mc.create
        r mc.exec watch x
        r set x 1
        assert_equal [r mc.getflags] 1
        r mc.delete
    }

    test {test persistent client multi/exec watch} {
        r set x 1
        r mc.delete
        r mc.create
        r mc.exec watch x
        r mc.exec multi
        r mc.exec get x
        r mc.exec get x
        assert { [r mc.exec exec] == {1 1} }
        r mc.delete
    }

    test {test persistent client multi/exec with watch failure} {
        r set x 1
        r mc.delete
        r mc.create
        r mc.exec watch x
        r mc.exec multi
        r mc.exec get x
        r mc.exec get x
        r set x 2
        assert { [r mc.exec exec] == {} }
        r mc.delete
    }

    test {test persistent client with blocking command} {
        r del x
        r mc.delete
        r mc.create
        set rd [redis_deferring_client]
        $rd mc.exec_async blpop x 0
        r lpush x 1
        assert_equal [$rd read] {x 1}
        r mc.delete
    }

    test {test persistent client with blocking command inside multi} {
        r del x
        r mc.create
        r set y 1
        r mc.exec multi
        r mc.exec_async blpop x 0
        r mc.exec get y
        assert { [r mc.exec exec] == {{} 1} }
        r mc.delete
    }

    test {test persistent client with user acl} {
        r del x
        r set x 1
        r mc.delete
        r mc.create
        r mc.reset_users
        r mc.set_user_acl client-user "~* &* +@all -set"
        assert_equal [r mc.exec_with_client_user get x] 1
        catch {r mc.exec_with_client_user set x 1} e
        assert_match {*NOPERM User client-user has no permissions to run the 'set' command*} $e
        r mc.free_users
    }

    test {test persistent client with user and ctx with user} {
        r del x
        r set x 1
        r mc.delete
        r mc.create
        r mc.reset_users
        r mc.set_user_acl client-user "~* &* +@all -set"
        r mc.set_user_acl context-user "~* &* +@all -get"
        catch {r mc.exec_with_client_and_ctx_user get x} e
        assert_match {*NOPERM User context-user has no permissions to run the 'get' command*} $e
        r mc.free_users
    }
}