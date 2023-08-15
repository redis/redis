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
        assert_error {*NOPERM*No permissions to access a key*} {r aclcheck.rm_call_with_errors set y 5 get}

        # rm call check for key permission (z: only READ)
        assert_error {*NOPERM*} {r aclcheck.rm_call set z 5}
        catch {r aclcheck.rm_call_with_errors set z 5} e
        assert_match {*NOPERM*No permissions to access a key*} $e
        assert_error {*NOPERM*} {r aclcheck.rm_call set z 6 get}
        assert_error {*NOPERM*No permissions to access a key*} {r aclcheck.rm_call_with_errors set z 6 get}

        # verify that new log entry added
        set entry [lindex [r ACL LOG] 0]
        assert {[dict get $entry username] eq {default}}
        assert {[dict get $entry context] eq {module}}
        assert {[dict get $entry object] eq {z}}
        assert {[dict get $entry reason] eq {key}}

        # rm call check for command permission
        r acl setuser default -set
        assert_error {*NOPERM*} {r aclcheck.rm_call set x 5}
        assert_error {*NOPERM*has no permissions to run the 'set' command*} {r aclcheck.rm_call_with_errors set x 5}

        # verify that new log entry added
        set entry [lindex [r ACL LOG] 0]
        assert {[dict get $entry username] eq {default}}
        assert {[dict get $entry context] eq {module}}
        assert {[dict get $entry object] eq {set}}
        assert {[dict get $entry reason] eq {command}}
    }

    test {test blocking of Commands outside of OnLoad} {
        assert_equal [r block.commands.outside.onload] OK
    }

    test {test users to have access to module commands having acl categories} {
        r acl SETUSER j1 on >password -@all +@WRITE
        r acl SETUSER j2 on >password -@all +@READ
        assert_equal [r acl DRYRUN j1 aclcheck.module.command.aclcategories.write] OK
        assert_equal [r acl DRYRUN j2 aclcheck.module.command.aclcategories.write.function.read.category] OK
        assert_equal [r acl DRYRUN j2 aclcheck.module.command.aclcategories.read.only.category] OK
    }

    test {Unload the module - aclcheck} {
        assert_equal {OK} [r module unload aclcheck]
    }
}

start_server {tags {"modules acl"}} {
    test {test existing users to have access to module commands loaded on runtime} {
        r acl SETUSER j3 on >password -@all +@WRITE
        assert_equal [r module load $testmodule] OK
        assert_equal [r acl DRYRUN j3 aclcheck.module.command.aclcategories.write] OK
        assert_equal {OK} [r module unload aclcheck]
    }
}

start_server {tags {"modules acl"}} {
    test {test existing users without permissions, do not have access to module commands loaded on runtime.} {
        r acl SETUSER j4 on >password -@all +@READ
        r acl SETUSER j5 on >password -@all +@WRITE
        assert_equal [r module load $testmodule] OK
        catch {r acl DRYRUN j4 aclcheck.module.command.aclcategories.write} e
        assert_equal {User j4 has no permissions to run the 'aclcheck.module.command.aclcategories.write' command} $e
        catch {r acl DRYRUN j5 aclcheck.module.command.aclcategories.write.function.read.category} e
        assert_equal {User j5 has no permissions to run the 'aclcheck.module.command.aclcategories.write.function.read.category' command} $e
    }

    test {test users without permissions, do not have access to module commands.} {
        r acl SETUSER j6 on >password -@all +@READ
        catch {r acl DRYRUN j6 aclcheck.module.command.aclcategories.write} e
        assert_equal {User j6 has no permissions to run the 'aclcheck.module.command.aclcategories.write' command} $e
        r acl SETUSER j7 on >password -@all +@WRITE
        catch {r acl DRYRUN j7 aclcheck.module.command.aclcategories.write.function.read.category} e
        assert_equal {User j7 has no permissions to run the 'aclcheck.module.command.aclcategories.write.function.read.category' command} $e
    }

    test {test if foocategory acl categories is added} {
        r acl SETUSER j8 on >password -@all +@foocategory
        assert_equal [r acl DRYRUN j8 aclcheck.module.command.test.add.new.aclcategories] OK
    }

    test {test permission compaction and simplification for categories added by a module} {
        r acl SETUSER j9 on >password -@all +@foocategory -@foocategory
        catch {r ACL GETUSER j9} res
        assert_equal {-@all -@foocategory} [lindex $res 5]
        assert_equal {OK} [r module unload aclcheck]
    }
}

start_server {tags {"modules acl"}} {
    test {test module load fails if exceeds the maximum number of adding acl categories} {
        assert_error {ERR Error loading the extension. Please check the server logs.} {r module load $testmodule 1}
    }
}
