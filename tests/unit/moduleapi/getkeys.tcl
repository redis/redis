set testmodule [file normalize tests/modules/getkeys.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test {COMMAND INFO correctly reports a movable keys module command} {
        set info [lindex [r command info getkeys.command] 0]

        assert_equal {movablekeys} [lindex $info 2]
        assert_equal {0} [lindex $info 3]
        assert_equal {0} [lindex $info 4]
        assert_equal {0} [lindex $info 5]
    }

    test {COMMAND GETKEYS correctly reports a movable keys module command} {
        r command getkeys getkeys.command arg1 arg2 key key1 arg3 key key2 key key3
    } {key1 key2 key3}

    test {RM_GetCommandKeys on non-existing command} {
        catch {r getkeys.introspect non-command key1 key2} e
        set _ $e
    } {*ENOENT*}

    test {RM_GetCommandKeys on built-in fixed keys command} {
        r getkeys.introspect set key1 value1
    } {key1}

    test {RM_GetCommandKeys on EVAL} {
        r getkeys.introspect eval "" 4 key1 key2 key3 key4 arg1 arg2
    } {key1 key2 key3 key4}

    test {RM_GetCommandKeys on a movable keys module command} {
        r getkeys.introspect getkeys.command arg1 arg2 key key1 arg3 key key2 key key3
    } {key1 key2 key3}

    test {RM_GetCommandKeys on a non-movable module command} {
        r getkeys.introspect getkeys.fixed arg1 key1 key2 key3 arg2
    } {key1 key2 key3}

    test {RM_GetCommandKeys with bad arity} {
        catch {r getkeys.introspect set key} e
        set _ $e
    } {*EINVAL*}
}
