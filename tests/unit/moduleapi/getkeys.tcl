set testmodule [file normalize tests/modules/getkeys.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test {COMMAND INFO correctly reports a movable keys module command} {
        set info [lindex [r command info getkeys.command] 0]

        assert_equal {module movablekeys} [lindex $info 2]
        assert_equal {0} [lindex $info 3]
        assert_equal {0} [lindex $info 4]
        assert_equal {0} [lindex $info 5]
    }

    test {COMMAND GETKEYS correctly reports a movable keys module command} {
        r command getkeys getkeys.command arg1 arg2 key key1 arg3 key key2 key key3
    } {key1 key2 key3}

    test {COMMAND GETKEYS correctly reports a movable keys module command using flags} {
        r command getkeys getkeys.command_with_flags arg1 arg2 key key1 arg3 key key2 key key3
    } {key1 key2 key3}

    test {COMMAND GETKEYSANDFLAGS correctly reports a movable keys module command not using flags} {
        r command getkeysandflags getkeys.command arg1 arg2 key key1 arg3 key key2
    } {{key1 {RW access update}} {key2 {RW access update}}}

    test {COMMAND GETKEYSANDFLAGS correctly reports a movable keys module command using flags} {
        r command getkeysandflags getkeys.command_with_flags arg1 arg2 key key1 arg3 key key2 key key3
    } {{key1 {RO access}} {key2 {RO access}} {key3 {RO access}}}

    test {RM_GetCommandKeys on non-existing command} {
        catch {r getkeys.introspect 0 non-command key1 key2} e
        set _ $e
    } {*ENOENT*}

    test {RM_GetCommandKeys on built-in fixed keys command} {
        r getkeys.introspect 0 set key1 value1
    } {key1}

    test {RM_GetCommandKeys on built-in fixed keys command with flags} {
        r getkeys.introspect 1 set key1 value1
    } {{key1 OW}}

    test {RM_GetCommandKeys on EVAL} {
        r getkeys.introspect 0 eval "" 4 key1 key2 key3 key4 arg1 arg2
    } {key1 key2 key3 key4}

    test {RM_GetCommandKeys on a movable keys module command} {
        r getkeys.introspect 0 getkeys.command arg1 arg2 key key1 arg3 key key2 key key3
    } {key1 key2 key3}

    test {RM_GetCommandKeys on a non-movable module command} {
        r getkeys.introspect 0 getkeys.fixed arg1 key1 key2 key3 arg2
    } {key1 key2 key3}

    test {RM_GetCommandKeys with bad arity} {
        catch {r getkeys.introspect 0 set key} e
        set _ $e
    } {*EINVAL*}

    # user that can only read from "read" keys, write to "write" keys, and read+write to "RW" keys
    r ACL setuser testuser +@all %R~read* %W~write* %RW~rw*

    test "module getkeys-api - ACL" {
        # legacy triple didn't provide flags, so they require both read and write
        assert_equal "OK" [r ACL DRYRUN testuser getkeys.command key rw]
        assert_match {*has no permissions to access the 'read' key*} [r ACL DRYRUN testuser getkeys.command key read]
        assert_match {*has no permissions to access the 'write' key*} [r ACL DRYRUN testuser getkeys.command key write]
    }

    test "module getkeys-api with flags - ACL" {
        assert_equal "OK" [r ACL DRYRUN testuser getkeys.command_with_flags key rw]
        assert_equal "OK" [r ACL DRYRUN testuser getkeys.command_with_flags key read]
        assert_match {*has no permissions to access the 'write' key*} [r ACL DRYRUN testuser getkeys.command_with_flags key write]
    }

    test "Unload the module - getkeys" {
        assert_equal {OK} [r module unload getkeys]
    }
}
