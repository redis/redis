set testmodule [file normalize tests/modules/keyspace_events.so]

tags "modules external:skip" {
    start_server [list overrides [list loadmodule "$testmodule"]] {

        # avoid using shared integers, to increase the chance of detection heap issues
        r config set maxmemory-policy allkeys-lru
        r config set maxmemory 1gb

        test {Test loaded key space event} {
            r set x 1
            r hset y f v
            r lpush z 1 2 3
            r sadd p 1 2 3
            r zadd t 1 f1 2 f2
            r xadd s * f v
            r debug reload
            assert_equal {1 x} [r keyspace.is_key_loaded x]
            assert_equal {1 y} [r keyspace.is_key_loaded y]
            assert_equal {1 z} [r keyspace.is_key_loaded z]
            assert_equal {1 p} [r keyspace.is_key_loaded p]
            assert_equal {1 t} [r keyspace.is_key_loaded t]
            assert_equal {1 s} [r keyspace.is_key_loaded s]
        }

        test {Nested multi due to RM_Call} {
            r del multi
            r del lua

            r set x 1
            r set x_copy 1
            r keyspace.del_key_copy x
            r keyspace.incr_case1 x
            r keyspace.incr_case2 x
            r keyspace.incr_case3 x
            assert_equal {} [r get multi]
            assert_equal {} [r get lua]
            r get x
        } {3}
        
        test {Nested multi due to RM_Call, with client MULTI} {
            r del multi
            r del lua

            r set x 1
            r set x_copy 1
            r multi
            r keyspace.del_key_copy x
            r keyspace.incr_case1 x
            r keyspace.incr_case2 x
            r keyspace.incr_case3 x
            r exec
            assert_equal {1} [r get multi]
            assert_equal {} [r get lua]
            r get x
        } {3}
        
        test {Nested multi due to RM_Call, with EVAL} {
            r del multi
            r del lua

            r set x 1
            r set x_copy 1
            r eval {
                redis.pcall('keyspace.del_key_copy', KEYS[1])
                redis.pcall('keyspace.incr_case1', KEYS[1])
                redis.pcall('keyspace.incr_case2', KEYS[1])
                redis.pcall('keyspace.incr_case3', KEYS[1])
            } 1 x
            assert_equal {} [r get multi]
            assert_equal {1} [r get lua]
            r get x
        } {3}

        test {Test module key space event} {
            r keyspace.notify x
            assert_equal {1 x} [r keyspace.is_module_key_notified x]
        }

        test "Keyspace notifications: module events test" {
            r config set notify-keyspace-events Kd
            r del x
            set rd1 [redis_deferring_client]
            assert_equal {1} [psubscribe $rd1 *]
            r keyspace.notify x
            assert_equal {pmessage * __keyspace@9__:x notify} [$rd1 read]
            $rd1 close
        }

        test "Keyspace notifications: unsubscribe removes handler" {
            r config set notify-keyspace-events KEA
            set before [r keyspace.callback_count]
            r set a 1
            r del a
            wait_for_condition 100 10 {
                [r keyspace.callback_count] > $before
            } else {
                fail "callback did not trigger"
            }
            set before_unsub [r keyspace.callback_count]
            r keyspace.unsubscribe 4  ;# REDISMODULE_NOTIFY_GENERIC
            r set a 1
            r del a
            set after_unsub [r keyspace.callback_count]
            assert_equal $before_unsub $after_unsub
        }

        test {Test expired key space event} {
            set prev_expired [s expired_keys]
            r set exp 1 PX 10
            wait_for_condition 100 10 {
                [s expired_keys] eq $prev_expired + 1
            } else {
                fail "key not expired"
            }
            assert_equal [r get testkeyspace:expired] 1
        }

        test "Subkey notification: subscribe starts callback" {
            r keyspace.subscribe_subkeys
            r keyspace.reset_subkey_events
            r config set notify-keyspace-events ""
        }
    
        test "Subkey notification: HSET triggers module subkey callback" {
            r keyspace.reset_subkey_events
            r hset myhash f1 v1 f2 v2
            set events [r keyspace.get_subkey_events]
            assert_equal 1 [llength $events]
            assert_equal "hset myhash 2 f1 f2" [lindex $events 0]
            r del myhash
        }

        test "Subkey notification: HDEL triggers module subkey callback" {
            r hset myhash f1 v1 f2 v2
            r keyspace.reset_subkey_events
            r hdel myhash f1
            set events [r keyspace.get_subkey_events]
            assert_equal 1 [llength $events]
            assert_equal "hdel myhash 1 f1" [lindex $events 0]
            r del myhash
        }

        test "Subkey notification: non-subkey event calls subkey callback with count=0" {
            r hset myhash f1 v1
            r keyspace.reset_subkey_events
            r del myhash
            set events [r keyspace.get_subkey_events]
            # DEL is NOTIFY_GENERIC — our callback is registered for
            # HASH|GENERIC, so it should be called with subkeys=NULL, count=0.
            assert_equal 1 [llength $events]
            assert_equal "del myhash 0" [lindex $events 0]
        }

        test "Subkey notification: module-triggered NotifyKeyspaceEventWithSubkeys" {
            r keyspace.reset_subkey_events
            r keyspace.notify_with_subkeys mykey sk1 sk2 sk3
            set events [r keyspace.get_subkey_events]
            assert_equal 1 [llength $events]
            assert_equal "module_subkey_event mykey 3 sk1 sk2 sk3" [lindex $events 0]
        }

        test "Subkey notification: lazy hash field expiry triggers hexpired with subkeys" {
            r debug set-active-expire 0
            r del myhash
            r hset myhash f1 v1 f2 v2 f3 v3
            r hpexpire myhash 10 FIELDS 2 f1 f2
            r keyspace.reset_subkey_events
            after 100
            r hmget myhash f1 f2
            assert_equal "hexpired myhash 2 f1 f2" [lindex [r keyspace.get_subkey_events] 0]
            r debug set-active-expire 1
        } {OK} {needs:debug}

        test "Subkey notification: active hash field expiry triggers hexpired with subkeys" {
            r del myhash
            r hset myhash f1 v1 f2 v2
            r keyspace.reset_subkey_events
            r hpexpire myhash 10 FIELDS 2 f1 f2
            # wait for active expiry to kick in
            wait_for_condition 50 100 {
                [r exists myhash] == 0
            } else {
                fail "Fields not expired by active expiry"
            }
            # fields order is undefined
            assert_match "hexpired myhash 2 f* f*" [lindex [r keyspace.get_subkey_events] 1]
            r del myhash
        }

        test "Subkey notification: unsubscribe stops callback and resubscribe resumes" {
            r keyspace.reset_subkey_events
            r hset myhash f1 v1
            set events [r keyspace.get_subkey_events]
            assert_equal 1 [llength $events]

            # Unsubscribe — events should stop
            r keyspace.unsubscribe_subkeys
            r keyspace.reset_subkey_events
            r hset myhash f2 v2
            set events [r keyspace.get_subkey_events]
            assert_equal 0 [llength $events]
            # active expire should not trigger subkey callback
            r hpexpire myhash 10 FIELDS 2 f1 f2
            wait_for_condition 50 100 {
                [r exists myhash] == 0
            } else {
                fail "Fields not expired by active expiry"
            }
            set events [r keyspace.get_subkey_events]
            assert_equal 0 [llength $events]

            # Re-subscribe — events should resume
            r keyspace.subscribe_subkeys
            r del myhash
            r hset myhash f1 v1 f2 v2
            r keyspace.reset_subkey_events
            r hpexpire myhash 10 FIELDS 2 f1 f2
            assert_match "hexpire myhash 2 f* f*" [lindex [r keyspace.get_subkey_events] 0]
            # active expire should also resume subkey callback
            wait_for_condition 50 100 {
                [r exists myhash] == 0
            } else {
                fail "Fields not expired by active expiry"
            }
            assert_match "hexpired myhash 2 f* f*" [lindex [r keyspace.get_subkey_events] 1]

            r keyspace.unsubscribe_subkeys
            r keyspace.reset_subkey_events
            r del myhash
        }

        test "Subkey notification: SUBKEYS_REQUIRED flag skips events without subkeys" {
            r keyspace.subscribe_require_subkeys
            r keyspace.reset_subkey_events

            # HSET has subkeys — should trigger callback
            r hset myhash f1 v1 f2 v2
            set events [r keyspace.get_subkey_events]
            assert_equal 1 [llength $events]
            assert_equal "hset myhash 2 f1 f2" [lindex $events 0]

            # DEL has no subkeys — the callback should be skipped.
            r keyspace.reset_subkey_events
            r del myhash
            set events [r keyspace.get_subkey_events]
            assert_equal 0 [llength $events]

            r keyspace.unsubscribe_require_subkeys
        }

        test "Unload the module - testkeyspace" {
            assert_equal {OK} [r module unload testkeyspace]
        }

        test "Verify RM_StringDMA with expiration are not causing invalid memory access" {
            assert_equal {OK} [r set x 1 EX 1]
        }
    }

    # Replication test: replica module receives subkey notifications
    start_server [list overrides [list loadmodule "$testmodule"]] {
        set master [srv 0 client]
        set master_host [srv 0 host]
        set master_port [srv 0 port]

        start_server [list overrides [list loadmodule "$testmodule"]] {
            set replica [srv 0 client]

            $replica replicaof $master_host $master_port
            wait_for_sync $replica

            test "Subkey notification: replica module receives subkey callback after replication" {
                $master keyspace.subscribe_subkeys
                $replica keyspace.subscribe_subkeys
                $replica keyspace.reset_subkey_events

                $master hset myhash f1 v1 f2 v2

                wait_for_ofs_sync $master $replica

                set events [$replica keyspace.get_subkey_events]
                assert_equal 1 [llength $events]
                assert_equal "hset myhash 2 f1 f2" [lindex $events 0]

                $master del myhash
                $master keyspace.unsubscribe_subkeys
                $replica keyspace.unsubscribe_subkeys
            }
        }
    }

    start_server {} {
        test {OnLoad failure will handle un-registration} {
            catch {r module load $testmodule noload}
            r set x 1
            r hset y f v
            r lpush z 1 2 3
            r sadd p 1 2 3
            r zadd t 1 f1 2 f2
            r xadd s * f v
            r ping
        }
    }
}
