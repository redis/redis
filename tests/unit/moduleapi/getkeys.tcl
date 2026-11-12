set testmodule [file normalize tests/modules/getkeys.so]

start_server {tags {"modules external:skip"}} {
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

    test "introspect with > MAX_KEYS_BUFFER keys triggers RM_GetCommandKeysWithFlags heap alloc" {
        set args {}
        for {set i 0} {$i < 7} {incr i} {
            lappend args key k$i
        }
        set reply [r getkeys.introspect 1 getkeys.command_with_flags {*}$args]
        assert_equal {{k0 RO} {k1 RO} {k2 RO} {k3 RO} {k4 RO} {k5 RO} {k6 RO}} $reply
    }

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

# Position callbacks must not change execution_nesting from IO threads while
# the main thread executes writes. Otherwise pending propagation can be skipped
# or flushed by an IO thread, causing assertions or replication crashes (#16042).
start_server {overrides {save ""} tags {iothreads repl network modules external:skip}} {
start_server {overrides {io-threads 4 save ""}} {
    set master [srv 0 client]
    set slave [srv -1 client]
    $master module load $testmodule
    $master module load [file normalize tests/modules/getchannels.so]

    test {Module key and channel position callbacks from IO threads do not corrupt execution nesting} {
        $slave replicaof [srv 0 host] [srv 0 port]
        wait_for_sync $slave

        set clients {}
        for {set i 0} {$i < 4} {incr i} {
            set rd [redis_deferring_client]
            $rd client info
            assert {[regexp {io-thread=(\d+)} [$rd read] - tid]}
            assert_morethan $tid 0
            lappend clients $rd
        }

        # IO threads resolving key/channel positions can race with the main thread
        # executing SET commands from other clients.
        # This stress test depends on timing and may miss the old race.
        set seconds 5 ;# run 5s stress test
        set batch 200
        set deadline [expr {[clock milliseconds] + $seconds * 1000}]
        while {[clock milliseconds] < $deadline} {
            foreach rd $clients {
                for {set j 0} {$j < $batch} {incr j} {
                    $rd set race:$j v
                    $rd getkeys.command key race:$j
                    $rd getchannels.command_with_keyspec publish literal race:$j
                }
            }
            foreach rd $clients {
                for {set j 0} {$j < $batch} {incr j} {
                    assert_equal OK [$rd read]
                    assert_equal [list race:$j] [$rd read]
                    assert_equal OK [$rd read]
                }
            }
        }

        foreach rd $clients { $rd close }
        wait_for_ofs_sync $master $slave
        assert_equal [$master debug digest] [$slave debug digest]
    }
}
}
