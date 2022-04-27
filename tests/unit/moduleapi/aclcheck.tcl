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
        assert {[dict get $entry reason] eq {command}}
    }

    test {test module check acl for key perm} {
        # give permission for SET and block all keys but x(READ+WRITE), y(WRITE), z(READ)
        r acl setuser default +set resetkeys ~x %W~y %R~z

        assert_equal [r aclcheck.set.check.key "*" x 5] OK
        catch {r aclcheck.set.check.key "*" v 5} e
        assert_match "*DENIED KEY*" $e

        assert_equal [r aclcheck.set.check.key "~" x 5] OK
        assert_equal [r aclcheck.set.check.key "~" y 5] OK
        assert_equal [r aclcheck.set.check.key "~" z 5] OK
        catch {r aclcheck.set.check.key "~" v 5} e
        assert_match "*DENIED KEY*" $e

        assert_equal [r aclcheck.set.check.key "W" y 5] OK
        catch {r aclcheck.set.check.key "W" v 5} e
        assert_match "*DENIED KEY*" $e

        assert_equal [r aclcheck.set.check.key "R" z 5] OK
        catch {r aclcheck.set.check.key "R" v 5} e
        assert_match "*DENIED KEY*" $e
    }

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
        # rm call check for key permission (x: READ + WRITE)
        assert_equal [r aclcheck.rm_call set x 5] OK
        assert_equal [r aclcheck.rm_call set x 6 get] 5

        # rm call check for key permission (y: only WRITE)
        assert_equal [r aclcheck.rm_call set y 5] OK
        assert_error {*NOPERM*} {r aclcheck.rm_call set y 5 get}
        assert_error {ERR acl verification failed, can't access at least one of the keys mentioned in the command arguments.} {r aclcheck.rm_call_with_errors set y 5 get}

        # rm call check for key permission (z: only READ)
        assert_error {*NOPERM*} {r aclcheck.rm_call set z 5}
        assert_error {ERR acl verification failed, can't access at least one of the keys mentioned in the command arguments.} {r aclcheck.rm_call_with_errors set z 5}
        assert_error {*NOPERM*} {r aclcheck.rm_call set z 6 get}
        assert_error {ERR acl verification failed, can't access at least one of the keys mentioned in the command arguments.} {r aclcheck.rm_call_with_errors set z 6 get}

        # verify that new log entry added
        set entry [lindex [r ACL LOG] 0]
        assert {[dict get $entry username] eq {default}}
        assert {[dict get $entry context] eq {module}}
        assert {[dict get $entry object] eq {z}}
        assert {[dict get $entry reason] eq {key}}

        # rm call check for command permission
        r acl setuser default -set
        catch {r aclcheck.rm_call set x 5} e
        assert_match {*NOPERM*} $e
        catch {r aclcheck.rm_call_with_errors set x 5} e
        assert_match {ERR acl verification failed, can't run this command or subcommand.} $e

        # verify that new log entry added
        set entry [lindex [r ACL LOG] 0]
        assert {[dict get $entry username] eq {default}}
        assert {[dict get $entry context] eq {module}}
        assert {[dict get $entry object] eq {set}}
        assert {[dict get $entry reason] eq {command}}
    }

    test "Unload the module - aclcheck" {
        assert_equal {OK} [r module unload aclcheck]
    }
}
