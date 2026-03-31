# Test for SetKeyMeta during keyspace notification (KSN) callbacks.
#
# This test loads a module that registers KSN callbacks for HASH, STRING,
# GENERIC, EXPIRED, and EVICTED events. The callback writes to key metadata
# (via RedisModule_SetKeyMeta), which may trigger kvobj reallocation.
# It exercises various commands across these notification types to catch
# regressions where the kvobj pointer becomes stale after a notification
# callback reallocates it.
#
# Important: each test uses a fresh key so that SetKeyMeta triggers an actual
# kvobj reallocation (the first metadata attachment grows the kvobj). We verify
# this by checking that the setcount increases after each command.
# Each test also validates that the metadata is properly accessible after the
# operation by reading it back via RedisModule_GetKeyMeta.

set testmodule [file normalize tests/modules/keymeta_notify.so]

start_server {tags {"modules" "external:skip"}} {
    r module load $testmodule

    # --- HASH notification tests ---
    # Each test uses a fresh key to ensure kvobj reallocation happens.

    test {HSETNX with SetKeyMeta in notification does not crash} {
        set before [r keymetanotify.setcount]
        r HSETNX hsetnx_key field1 value1
        assert_equal [r HGET hsetnx_key field1] "value1"
        assert_equal [r keymetanotify.get hsetnx_key] "notified"
        # Verify SetKeyMeta was called (reallocation happened on first call)
        assert {[r keymetanotify.setcount] > $before}

        # Second HSETNX on same field (no-op, field exists) - in-place update
        r HSETNX hsetnx_key field1 value2
        assert_equal [r HGET hsetnx_key field1] "value1"

        # HSETNX on a new field in the same hash
        r HSETNX hsetnx_key field2 value2
        assert_equal [r HGET hsetnx_key field2] "value2"
        assert_equal [r HLEN hsetnx_key] 2
    }

    test {HSET with SetKeyMeta in notification does not crash} {
        set before [r keymetanotify.setcount]
        r HSET hset_key f1 v1
        assert_equal [r HGET hset_key f1] "v1"
        assert_equal [r keymetanotify.get hset_key] "notified"
        assert {[r keymetanotify.setcount] > $before}

        # Multiple fields on same key (in-place metadata update)
        r HSET hset_key f2 v2 f3 v3
        assert_equal [r HLEN hset_key] 3
    }

    test {HMSET with SetKeyMeta in notification does not crash} {
        set before [r keymetanotify.setcount]
        r HMSET hmset_key f1 v1 f2 v2
        assert_equal [r HGET hmset_key f1] "v1"
        assert_equal [r HGET hmset_key f2] "v2"
        assert_equal [r keymetanotify.get hmset_key] "notified"
        assert {[r keymetanotify.setcount] > $before}
    }

    test {HINCRBY with SetKeyMeta in notification does not crash} {
        # Use a fresh key - HINCRBY creates it with value 5
        set before [r keymetanotify.setcount]
        r HINCRBY hincrby_key counter 5
        assert_equal [r HGET hincrby_key counter] "5"
        assert_equal [r keymetanotify.get hincrby_key] "notified"
        assert {[r keymetanotify.setcount] > $before}
    }

    test {HINCRBYFLOAT with SetKeyMeta in notification does not crash} {
        # Use a fresh key - HINCRBYFLOAT creates it with value 1.5
        set before [r keymetanotify.setcount]
        r HINCRBYFLOAT hincrbyfloat_key value 1.5
        assert_equal [r HGET hincrbyfloat_key value] "1.5"
        assert_equal [r keymetanotify.get hincrbyfloat_key] "notified"
        assert {[r keymetanotify.setcount] > $before}
    }

    test {Multiple HSETNX on new keys with SetKeyMeta does not crash} {
        set before [r keymetanotify.setcount]
        for {set i 0} {$i < 100} {incr i} {
            r HSETNX "stresskey:$i" field "value$i"
        }
        for {set i 0} {$i < 100} {incr i} {
            assert_equal [r HGET "stresskey:$i" field] "value$i"
            assert_equal [r keymetanotify.get "stresskey:$i"] "notified"
        }
        # All 100 keys should have triggered SetKeyMeta
        assert {[r keymetanotify.setcount] >= $before + 100}
    }

    # --- STRING notification tests ---
    # Each test uses a fresh key for actual kvobj reallocation.

    test {SET with SetKeyMeta in notification does not crash} {
        set before [r keymetanotify.setcount]
        r SET set_key hello
        assert_equal [r GET set_key] "hello"
        assert_equal [r keymetanotify.get set_key] "notified"
        assert {[r keymetanotify.setcount] > $before}
    }

    test {APPEND with SetKeyMeta in notification does not crash} {
        # APPEND on nonexistent key creates it
        set before [r keymetanotify.setcount]
        r APPEND append_key "hello"
        assert_equal [r GET append_key] "hello"
        assert_equal [r keymetanotify.get append_key] "notified"
        assert {[r keymetanotify.setcount] > $before}
    }

    test {INCR with SetKeyMeta in notification does not crash} {
        # INCR on nonexistent key creates it with value 1
        set before [r keymetanotify.setcount]
        r INCR incr_key
        assert_equal [r GET incr_key] "1"
        assert_equal [r keymetanotify.get incr_key] "notified"
        assert {[r keymetanotify.setcount] > $before}
    }

    test {INCRBY with SetKeyMeta in notification does not crash} {
        set before [r keymetanotify.setcount]
        r INCRBY incrby_key 5
        assert_equal [r GET incrby_key] "5"
        assert_equal [r keymetanotify.get incrby_key] "notified"
        assert {[r keymetanotify.setcount] > $before}
    }

    test {INCRBYFLOAT with SetKeyMeta in notification does not crash} {
        set before [r keymetanotify.setcount]
        r SET incrbyfloat_key 10.5
        r INCRBYFLOAT incrbyfloat_key 1.5
        assert_equal [r GET incrbyfloat_key] "12"
        assert_equal [r keymetanotify.get incrbyfloat_key] "notified"
        assert {[r keymetanotify.setcount] > $before}
    }

    test {GETSET with SetKeyMeta in notification does not crash} {
        set before [r keymetanotify.setcount]
        r SET getset_key "old"
        r GETSET getset_key "new"
        assert_equal [r GET getset_key] "new"
        assert_equal [r keymetanotify.get getset_key] "notified"
        assert {[r keymetanotify.setcount] > $before}
    }

    test {SETRANGE with SetKeyMeta in notification does not crash} {
        set before [r keymetanotify.setcount]
        r SET setrange_key "Hello World"
        r SETRANGE setrange_key 6 "Redis"
        assert_equal [r GET setrange_key] "Hello Redis"
        assert_equal [r keymetanotify.get setrange_key] "notified"
        assert {[r keymetanotify.setcount] > $before}
    }

    # --- GENERIC notification tests ---

    test {DEL with SetKeyMeta in notification does not crash} {
        r SET del_key "value"
        assert_equal [r keymetanotify.get del_key] "notified"
        r DEL del_key
        assert_equal [r EXISTS del_key] 0
    }

    test {RENAME with SetKeyMeta in notification does not crash} {
        r SET rename_src "value"
        r RENAME rename_src rename_dst
        assert_equal [r GET rename_dst] "value"
        assert_equal [r EXISTS rename_src] 0
    }

    test {RESTORE with SetKeyMeta in notification does not crash} {
        r SET restore_src "hello"
        set dump [r DUMP restore_src]
        r DEL restore_src
        set before [r keymetanotify.setcount]
        r RESTORE restore_dst 0 $dump
        assert_equal [r GET restore_dst] "hello"
        assert {[r keymetanotify.setcount] > $before}
    }

    test {RESTORE REPLACE with SetKeyMeta in notification does not crash} {
        # Create a key with metadata already attached
        r SET restore_replace_src "hello"
        assert_equal [r keymetanotify.get restore_replace_src] "notified"
        set dump [r DUMP restore_replace_src]
        # Create a destination key that already exists (with metadata)
        r SET restore_replace_dst "old_value"
        assert_equal [r keymetanotify.get restore_replace_dst] "notified"
        set before [r keymetanotify.setcount]
        # RESTORE REPLACE overwrites the existing key, triggering delete + load
        r RESTORE restore_replace_dst 0 $dump REPLACE
        assert_equal [r GET restore_replace_dst] "hello"
        assert {[r keymetanotify.setcount] > $before}
    }

    test {EXPIRE and key expiry with SetKeyMeta in notification does not crash} {
        r SET expire_key "value"
        assert_equal [r keymetanotify.get expire_key] "notified"
        r PEXPIRE expire_key 50
        after 100
        assert_equal [r EXISTS expire_key] 0
    }

    test {DEBUG RELOAD with SetKeyMeta in notification does not crash} {
        r SET reload_key "value"
        assert_equal [r keymetanotify.get reload_key] "notified"
        r DEBUG RELOAD
        # After reload, keys are restored from RDB triggering LOADED notifications.
        # The module setcount counter resets on reload, so just verify it is > 0
        # (meaning SetKeyMeta was called during RDB loading).
        assert_equal [r GET reload_key] "value"
        assert {[r keymetanotify.setcount] > 0}
    }

    test {SetKeyMeta notification count is tracked} {
        set count [r keymetanotify.setcount]
        assert {$count > 0}
    }
}
