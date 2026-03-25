# Test for SetKeyMeta during keyspace notification (KSN) callbacks.
#
# This test loads a module that registers KSN callbacks for HASH, STRING,
# GENERIC, EXPIRED, and EVICTED events. The callback writes to key metadata
# (via RedisModule_SetKeyMeta), which may trigger kvobj reallocation.
# It exercises various commands across these notification types to catch
# regressions where the kvobj pointer becomes stale after a notification
# callback reallocates it.

set testmodule [file normalize tests/modules/keymeta_notify.so]

start_server {tags {"modules" "external:skip"}} {
    r module load $testmodule

    test {HSETNX with SetKeyMeta in notification works correctly} {
        r HSETNX mykey field1 value1

        # Verify the hash is valid and accessible
        assert_equal [r HGET mykey field1] "value1"

        # Verify metadata was set by the notification callback
        assert_equal [r keymetanotify.get mykey] "notified"

        # Second HSETNX on same field (no-op, field exists)
        r HSETNX mykey field1 value2
        assert_equal [r HGET mykey field1] "value1"

        # HSETNX on a new field in the same hash
        r HSETNX mykey field2 value2
        assert_equal [r HGET mykey field2] "value2"
        assert_equal [r HLEN mykey] 2

        # Verify the hash is still fully functional
        assert_equal [r keymetanotify.get mykey] "notified"
    }

    test {HSET with SetKeyMeta in notification works correctly} {
        r DEL mykey2
        r HSET mykey2 f1 v1
        assert_equal [r HGET mykey2 f1] "v1"
        assert_equal [r keymetanotify.get mykey2] "notified"

        # Multiple fields
        r HSET mykey2 f2 v2 f3 v3
        assert_equal [r HLEN mykey2] 3
        assert_equal [r keymetanotify.get mykey2] "notified"
    }

    test {HMSET with SetKeyMeta in notification works correctly} {
        r DEL mykey3
        r HMSET mykey3 f1 v1 f2 v2
        assert_equal [r HGET mykey3 f1] "v1"
        assert_equal [r HGET mykey3 f2] "v2"
        assert_equal [r keymetanotify.get mykey3] "notified"
    }

    test {HINCRBY with SetKeyMeta in notification works correctly} {
        r DEL mykey4
        r HSET mykey4 counter 10
        r HINCRBY mykey4 counter 5
        assert_equal [r HGET mykey4 counter] "15"
        assert_equal [r keymetanotify.get mykey4] "notified"
    }

    test {HINCRBYFLOAT with SetKeyMeta in notification works correctly} {
        r DEL mykey5
        r HSET mykey5 value 10.5
        r HINCRBYFLOAT mykey5 value 1.5
        assert_equal [r HGET mykey5 value] "12"
        assert_equal [r keymetanotify.get mykey5] "notified"
    }

    test {Multiple HSETNX on new keys with SetKeyMeta does not crash} {
        # Stress test: create many keys via HSETNX
        for {set i 0} {$i < 100} {incr i} {
            r HSETNX "stresskey:$i" field "value$i"
        }

        # Verify all keys are valid
        for {set i 0} {$i < 100} {incr i} {
            assert_equal [r HGET "stresskey:$i" field] "value$i"
            assert_equal [r keymetanotify.get "stresskey:$i"] "notified"
        }
    }

    # --- STRING notification tests ---

    test {SET with SetKeyMeta in notification does not crash} {
        r SET strkey1 hello
        assert_equal [r GET strkey1] "hello"
        assert_equal [r keymetanotify.get strkey1] "notified"
    }

    test {APPEND with SetKeyMeta in notification does not crash} {
        r DEL strkey2
        r SET strkey2 "hello"
        r APPEND strkey2 " world"
        assert_equal [r GET strkey2] "hello world"
        assert_equal [r keymetanotify.get strkey2] "notified"
    }

    test {INCR with SetKeyMeta in notification does not crash} {
        r DEL strkey3
        r SET strkey3 10
        r INCR strkey3
        assert_equal [r GET strkey3] "11"
        assert_equal [r keymetanotify.get strkey3] "notified"
    }

    test {INCRBY with SetKeyMeta in notification does not crash} {
        r DEL strkey4
        r SET strkey4 10
        r INCRBY strkey4 5
        assert_equal [r GET strkey4] "15"
        assert_equal [r keymetanotify.get strkey4] "notified"
    }

    test {INCRBYFLOAT with SetKeyMeta in notification does not crash} {
        r DEL strkey5
        r SET strkey5 10.5
        r INCRBYFLOAT strkey5 1.5
        assert_equal [r GET strkey5] "12"
        assert_equal [r keymetanotify.get strkey5] "notified"
    }

    test {GETSET with SetKeyMeta in notification does not crash} {
        r DEL strkey6
        r SET strkey6 "old"
        r GETSET strkey6 "new"
        assert_equal [r GET strkey6] "new"
        assert_equal [r keymetanotify.get strkey6] "notified"
    }

    test {SETRANGE with SetKeyMeta in notification does not crash} {
        r DEL strkey7
        r SET strkey7 "Hello World"
        r SETRANGE strkey7 6 "Redis"
        assert_equal [r GET strkey7] "Hello Redis"
        assert_equal [r keymetanotify.get strkey7] "notified"
    }

    # --- GENERIC notification tests ---

    test {DEL with SetKeyMeta in notification does not crash} {
        r SET delkey "value"
        assert_equal [r keymetanotify.get delkey] "notified"
        r DEL delkey
        # After DEL the key is gone, metadata should be gone too
        assert_equal [r EXISTS delkey] 0
    }

    test {RENAME with SetKeyMeta in notification does not crash} {
        r SET renamekey1 "value"
        r RENAME renamekey1 renamekey2
        assert_equal [r GET renamekey2] "value"
        assert_equal [r EXISTS renamekey1] 0
    }

    test {EXPIRE and key expiry with SetKeyMeta in notification does not crash} {
        r SET expkey "value"
        assert_equal [r keymetanotify.get expkey] "notified"
        r PEXPIRE expkey 50
        # Wait for expiration
        after 100
        assert_equal [r EXISTS expkey] 0
    }

    test {SetKeyMeta notification count is tracked} {
        # The setcount should be > 0 since we've been setting metadata
        set count [r keymetanotify.setcount]
        assert {$count > 0}
    }
}
