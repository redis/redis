set testmodule [file normalize tests/modules/aclcheck.so]

start_server {tags {"modules acl"}} {
    r module load $testmodule

    test {test module check acl for command perm} {
        # by default all commands allowed
        assert_equal [r aclcheck.rm_call.check.cmd set x 5] OK
        # block SET command for user
        r acl setuser default -set
        catch {r aclcheck.rm_call.check.cmd set x 5} e
        assert_match {*DENIED CMD*} $e

        # verify that new log entry added
        set entry [lindex [r ACL LOG] 0]
        assert {[dict get $entry username] eq {default}}
        assert {[dict get $entry context] eq {module}}
        assert {[dict get $entry object] eq {set}}
    }

    test {test module check acl for key perm} {
        # give permission for SET and block all keys but x
        r acl setuser default +set resetkeys ~x
        assert_equal [r aclcheck.set.check.key x 5] OK
        catch {r aclcheck.set.check.key y 5} e
        set e
    } {*DENIED KEY*}

    test {test module check acl for module user} {
        # the module user has access to all keys
        assert_equal [r aclcheck.rm_call.check.cmd.module.user set y 5] OK
    }

    test {test module check acl for channel perm} {
        # block all channels but ch1
        r acl setuser default resetchannels &ch1
        assert_equal [r aclcheck.publish.check.channel ch1 msg] 0
        catch {r aclcheck.publish.check.channel ch2 msg} e
        set e
    } {*DENIED CHANNEL*}

    test {test module check acl in rm_call} {
        # rm call check for key permission (x can be accessed)
        assert_equal [r aclcheck.rm_call set x 5] OK
        # rm call check for key permission (y can't be accessed)
        catch {r aclcheck.rm_call set y 5} e
        assert_match {*NOPERM*} $e

        # verify that new log entry added
        set entry [lindex [r ACL LOG] 0]
        assert {[dict get $entry username] eq {default}}
        assert {[dict get $entry context] eq {module}}
        assert {[dict get $entry object] eq {y}}

        # rm call check for command permission
        r acl setuser default -set
        catch {r aclcheck.rm_call set x 5} e
        assert_match {*NOPERM*} $e

        # verify that new log entry added
        set entry [lindex [r ACL LOG] 0]
        assert {[dict get $entry username] eq {default}}
        assert {[dict get $entry context] eq {module}}
        assert {[dict get $entry object] eq {set}}
    }
}
