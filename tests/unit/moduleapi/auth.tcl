set testmodule [file normalize tests/modules/auth.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test {Modules can create a user that can be authenticated} {
        # Make sure we start authenticated with default user
        r auth default ""
        assert_equal [r acl whoami] "default"
        r auth.createmoduleuser

        set id [r auth.authmoduleuser]
        assert_equal [r client id] $id

        # Verify returned id is the same as our current id and
        # we are authenticated with the specified user
        assert_equal [r acl whoami] "global"
    }

    test {De-authenticating clients is tracked and kills clients} {
        assert_equal [r auth.changecount] 0
        r auth.createmoduleuser

        # Catch the I/O exception that was thrown when Redis
        # disconnected with us. 
        catch { [r ping] } e
        assert_match {*I/O*} $e

        # Check that a user change was registered
        assert_equal [r auth.changecount] 1
    }

    test {Modules can't authenticate with ACLs users that dont exist} {
        catch { [r auth.authrealuser auth-module-test-fake] } e
        assert_match {*Invalid user*} $e
    }

    test {Modules can authenticate with ACL users} {
        assert_equal [r acl whoami] "default"

        # Create user to auth into
        r acl setuser auth-module-test on allkeys allcommands

        set id [r auth.authrealuser auth-module-test]

        # Verify returned id is the same as our current id and
        # we are authenticated with the specified user
        assert_equal [r client id] $id
        assert_equal [r acl whoami] "auth-module-test"
    }

    test {Client callback is called on user switch} {
        assert_equal [r auth.changecount] 0

        # Auth again and validate change count
        r auth.authrealuser auth-module-test
        assert_equal [r auth.changecount] 1

        # Re-auth with the default user
        r auth default ""
        assert_equal [r auth.changecount] 1
        assert_equal [r acl whoami] "default"

        # Re-auth with the default user again, to
        # verify the callback isn't fired again
        r auth default ""
        assert_equal [r auth.changecount] 0
        assert_equal [r acl whoami] "default"
    }

}