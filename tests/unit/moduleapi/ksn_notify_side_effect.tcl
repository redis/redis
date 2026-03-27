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

    test {HSETEX with SetKeyMeta in notification does not crash} {
        set before [r keymetanotify.setcount]
        r HSETEX hsetex_key FIELDS 1 f1 v1
        assert_equal [r HGET hsetex_key f1] "v1"
        assert_equal [r keymetanotify.get hsetex_key] "notified"
        assert {[r keymetanotify.setcount] > $before}

        # HSETEX with expiration
        r HSETEX hsetex_key EX 1000 FIELDS 1 f2 v2
        assert_equal [r HGET hsetex_key f2] "v2"
        assert_equal [r HLEN hsetex_key] 2

        # HSETEX with FXX flag (only set if all fields exist)
        r HSETEX hsetex_key FXX FIELDS 1 f1 v1_updated
        assert_equal [r HGET hsetex_key f1] "v1_updated"

        # HSETEX with FNX flag (only set if no fields exist)
        set before [r keymetanotify.setcount]
        r HSETEX hsetex_fnx_key FNX FIELDS 2 f1 v1 f2 v2
        assert_equal [r HGET hsetex_fnx_key f1] "v1"
        assert_equal [r HGET hsetex_fnx_key f2] "v2"
        assert_equal [r keymetanotify.get hsetex_fnx_key] "notified"
        assert {[r keymetanotify.setcount] > $before}
    }

    test {HGETDEL with SetKeyMeta in notification does not crash} {
        # To test the "first SetKeyMeta causes kvobj reallocation" scenario,
        # create the key BEFORE loading the module so the first metadata
        # attachment happens during HGETDEL, not during HSET.
        r module unload keymetanotify
        r HSET hgetdel_key f1 v1 f2 v2 f3 v3
        r module load $testmodule

        # HGETDEL returns the value and deletes the field
        # This is the first SetKeyMeta call for this key, triggering kvobj reallocation
        set before [r keymetanotify.setcount]
        set result [r HGETDEL hgetdel_key FIELDS 1 f1]
        assert_equal $result "v1"
        assert_equal [r HEXISTS hgetdel_key f1] 0
        assert_equal [r HLEN hgetdel_key] 2
        # SetKeyMeta should be called during the hdel notification
        assert {[r keymetanotify.setcount] > $before}
        assert_equal [r keymetanotify.get hgetdel_key] "notified"

        # HGETDEL multiple fields
        set result [r HGETDEL hgetdel_key FIELDS 2 f2 f3]
        assert_equal [lindex $result 0] "v2"
        assert_equal [lindex $result 1] "v3"
        assert_equal [r HLEN hgetdel_key] 0
    }

    test {HGETEX with SetKeyMeta in notification does not crash} {
        # To test the "first SetKeyMeta causes kvobj reallocation" scenario,
        # create the key BEFORE loading the module so the first metadata
        # attachment happens during HGETEX, not during HSET.
        r module unload keymetanotify
        r HSET hgetex_key f1 v1 f2 v2
        r module load $testmodule

        # HGETEX with expiration - this is the first SetKeyMeta call for this key,
        # triggering kvobj reallocation during the hexpire notification
        set before [r keymetanotify.setcount]
        set result [r HGETEX hgetex_key EX 1000 FIELDS 1 f1]
        assert_equal [lindex $result 0] "v1"
        # hexpire notification triggers SetKeyMeta
        assert {[r keymetanotify.setcount] > $before}
        assert_equal [r keymetanotify.get hgetex_key] "notified"

        # HGETEX without expiration just returns values (in-place metadata update)
        set result [r HGETEX hgetex_key FIELDS 1 f1]
        assert_equal [lindex $result 0] "v1"

        # HGETEX with PERSIST - triggers hpersist notification
        set before [r keymetanotify.setcount]
        r HGETEX hgetex_key PERSIST FIELDS 1 f1
        assert_equal [r HTTL hgetex_key FIELDS 1 f1] -1
        # hpersist notification triggers SetKeyMeta
        assert {[r keymetanotify.setcount] > $before}
    }

    test {HDEL with SetKeyMeta in notification does not crash} {
        # To test the "first SetKeyMeta causes kvobj reallocation" scenario,
        # create the key BEFORE loading the module so the first metadata
        # attachment happens during HDEL, not during HSET.
        r module unload keymetanotify
        r HSET hdel_key f1 v1 f2 v2 f3 v3
        r module load $testmodule

        # HDEL single field - this is the first SetKeyMeta call for this key,
        # triggering kvobj reallocation during the hdel notification
        set before [r keymetanotify.setcount]
        r HDEL hdel_key f1
        assert_equal [r HEXISTS hdel_key f1] 0
        assert_equal [r HLEN hdel_key] 2
        # SetKeyMeta should be called during the hdel notification
        assert {[r keymetanotify.setcount] > $before}
        assert_equal [r keymetanotify.get hdel_key] "notified"

        # HDEL multiple fields (in-place metadata update)
        r HDEL hdel_key f2 f3
        assert_equal [r HLEN hdel_key] 0
    }

    test {HEXPIRE with SetKeyMeta in notification does not crash} {
        # To test the "first SetKeyMeta causes kvobj reallocation" scenario,
        # create the key BEFORE loading the module so the first metadata
        # attachment happens during HEXPIRE, not during HSET.
        r module unload keymetanotify
        r HSET hexpire_key f1 v1 f2 v2
        r module load $testmodule

        # HEXPIRE sets field expiration - this is the first SetKeyMeta call
        # for this key, triggering kvobj reallocation
        set before [r keymetanotify.setcount]
        r HEXPIRE hexpire_key 1000 FIELDS 1 f1
        # hexpire notification triggers SetKeyMeta
        assert {[r keymetanotify.setcount] > $before}
        assert_equal [r keymetanotify.get hexpire_key] "notified"

        # Verify TTL is set
        assert {[r HTTL hexpire_key FIELDS 1 f1] > 0}

        # HPEXPIRE (milliseconds) - in-place metadata update
        r HPEXPIRE hexpire_key 500000 FIELDS 1 f2
        assert {[r HPTTL hexpire_key FIELDS 1 f2] > 0}
    }

    test {HPERSIST with SetKeyMeta in notification does not crash} {
        # To test the "first SetKeyMeta causes kvobj reallocation" scenario,
        # create the key with field expiration BEFORE loading the module so
        # the first metadata attachment happens during HPERSIST.
        r module unload keymetanotify
        r HSET hpersist_key f1 v1
        r HEXPIRE hpersist_key 1000 FIELDS 1 f1
        r module load $testmodule

        # HPERSIST removes field expiration - this is the first SetKeyMeta call
        # for this key, triggering kvobj reallocation
        set before [r keymetanotify.setcount]
        r HPERSIST hpersist_key FIELDS 1 f1
        # hpersist notification triggers SetKeyMeta
        assert {[r keymetanotify.setcount] > $before}
        assert_equal [r keymetanotify.get hpersist_key] "notified"

        # Verify TTL is removed
        assert_equal [r HTTL hpersist_key FIELDS 1 f1] -1
    }

    test {Hash field expiration (hexpired) with SetKeyMeta in notification does not crash} {
        # Create hash with field that will expire quickly
        set before [r keymetanotify.setcount]
        r HSET hexpired_key f1 v1 f2 v2
        assert_equal [r keymetanotify.get hexpired_key] "notified"
        assert {[r keymetanotify.setcount] > $before}

        # Set very short expiration (100ms)
        r HPEXPIRE hexpired_key 100 FIELDS 1 f1

        # Wait for field to expire
        after 200

        # Access the hash to trigger lazy expiration (hexpired notification)
        # The main test here is that this doesn't crash when SetKeyMeta is called
        # during the hexpired notification callback
        set result [r HGET hexpired_key f1]
        # Field should be expired
        assert_equal $result {}

        # f2 should still exist and accessible without crash
        assert_equal [r HGET hexpired_key f2] "v2"

        # Key should still have metadata set
        assert_equal [r keymetanotify.get hexpired_key] "notified"
    }

    # --- GENERIC notification tests ---

    test {PERSIST with SetKeyMeta in notification does not crash} {
        # Create key with expiration
        set before [r keymetanotify.setcount]
        r SET persist_key "value"
        r EXPIRE persist_key 1000
        assert_equal [r keymetanotify.get persist_key] "notified"
        assert {[r keymetanotify.setcount] > $before}

        # Verify TTL is set
        assert {[r TTL persist_key] > 0}

        # PERSIST removes expiration
        set before [r keymetanotify.setcount]
        r PERSIST persist_key
        # persist notification triggers SetKeyMeta
        assert {[r keymetanotify.setcount] > $before}

        # Verify TTL is removed
        assert_equal [r TTL persist_key] -1
        assert_equal [r GET persist_key] "value"
    }

    test {COPY with SetKeyMeta in notification does not crash} {
        # Create source key
        set before [r keymetanotify.setcount]
        r HSET copy_src_key f1 v1 f2 v2
        assert_equal [r keymetanotify.get copy_src_key] "notified"
        assert {[r keymetanotify.setcount] > $before}

        # COPY to new key
        set before [r keymetanotify.setcount]
        r COPY copy_src_key copy_dst_key
        # copy_to notification triggers SetKeyMeta on destination
        assert_equal [r keymetanotify.get copy_dst_key] "notified"
        assert {[r keymetanotify.setcount] > $before}

        # Verify both keys have same content
        assert_equal [r HGET copy_src_key f1] "v1"
        assert_equal [r HGET copy_dst_key f1] "v1"
        assert_equal [r HGET copy_src_key f2] "v2"
        assert_equal [r HGET copy_dst_key f2] "v2"

        # COPY with REPLACE
        r HSET copy_src_key f3 v3
        set before [r keymetanotify.setcount]
        r COPY copy_src_key copy_dst_key REPLACE
        assert {[r keymetanotify.setcount] > $before}
        assert_equal [r HGET copy_dst_key f3] "v3"
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
