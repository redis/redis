set testmodule [file normalize tests/modules/postnotifications.so]

# ----------------------------------------------------------------------------
# Both post-notification APIs (RM_AddPostNotificationJob and
# RM_AddPostNotificationJobForKey) share the same .so. Tests that exercise
# behavior identical across both APIs — i.e. anything that doesn't depend on
# the per-key API's distinct firing point inside MULTI/EXEC — are written as a
# `foreach` over (regular, perkey) so the same assertions hold for both.
# API-specific tests stay as standalone tests.
# ----------------------------------------------------------------------------

tags "modules external:skip" {
    start_server {} {
        r module load $testmodule with_key_events

        # Common: a single SET fires the post-notification job registered by
        # the KSN handler, and the side effect is propagated wrapped with the
        # SET in one implicit MULTI/EXEC. The second SET overwrites the key,
        # which also fires the RedisModuleEvent_Key.before_overwritten server
        # event (registered first during dbGenericDelete inside setKey, before
        # notifyKeyspaceEvent).
        foreach {api key} {
            regular  string_x
            perkey   batched_a
        } {
            test "Test write on post notification callback ($api API)" {
                r flushall
                set repl [attach_to_replication_stream]

                r set $key 1
                if {$api eq "regular"} {
                    assert_equal {1} [r get string_changed{string_x}]
                    assert_equal {1} [r get string_total]
                } else {
                    assert_equal {batched_a} [r lrange batched_keys 0 -1]
                }

                r set $key 2
                if {$api eq "regular"} {
                    assert_equal {2} [r get string_changed{string_x}]
                    assert_equal {2} [r get string_total]
                    # the {lpush before_overwritten string_x} is a post notification job registered when 'string_x' was overwritten
                    assert_replication_stream $repl {
                        {multi}
                        {select *}
                        {set string_x 1}
                        {incr string_changed{string_x}}
                        {incr string_total}
                        {exec}
                        {multi}
                        {set string_x 2}
                        {lpush before_overwritten string_x}
                        {incr string_changed{string_x}}
                        {incr string_total}
                        {exec}
                    }
                } else {
                    assert_equal {batched_a batched_a} [r lrange batched_keys 0 -1]
                    assert_replication_stream $repl {
                        {multi}
                        {select *}
                        {set batched_a 1}
                        {lpush batched_keys batched_a}
                        {exec}
                        {multi}
                        {set batched_a 2}
                        {lpush batched_keys batched_a}
                        {lpush before_overwritten batched_a}
                        {exec}
                    }
                }
                close_replication_stream $repl
            }
        }

        # Common: an RM_Call from a module thread (after ThreadSafeContextLock)
        # goes through call() and sets server.executing_client, so both APIs
        # accept the registration.
        foreach {api key} {
            regular  string_x
            perkey   batched_a
        } {
            test "Test write on post notification callback from module thread ($api API)" {
                r flushall
                set repl [attach_to_replication_stream]

                assert_equal {OK} [r postnotification.async_set $key]

                if {$api eq "regular"} {
                    assert_equal {1} [r get string_changed{string_x}]
                    assert_equal {1} [r get string_total]
                    assert_replication_stream $repl {
                        {multi}
                        {select *}
                        {set string_x 1}
                        {incr string_changed{string_x}}
                        {incr string_total}
                        {exec}
                    }
                } else {
                    assert_equal $key [r lindex batched_keys 0]
                    assert_replication_stream $repl {
                        {multi}
                        {select *}
                        {set batched_a 1}
                        {lpush batched_keys batched_a}
                        {exec}
                    }
                }
                close_replication_stream $repl
            }
        }

        # Regular-only: active expire fires from cron with no executing_client,
        # so the per-key API's single-key guard refuses the registration.
        test {Test active expire} {
            r flushall
            set repl [attach_to_replication_stream]

            r set x 1
            r pexpire x 10

            wait_for_condition 100 50 {
                [r keys expired] == {expired}
            } else {
                puts [r keys *]
                fail "Failed waiting for x to expired"
            }

            # the {lpush before_expired x} is a post notification job registered before 'x' got expired
            assert_replication_stream $repl {
                {select *}
                {set x 1}
                {pexpireat x *}
                {multi}
                {del x}
                {lpush before_expired x}
                {incr expired}
                {exec}
            }
            close_replication_stream $repl
        }

        # Common: lazy DEL on key access fires NOTIFY_EXPIRED with
        # server.executing_client still set, so a post-notification job
        # registered from inside the handler is queued and propagated as part
        # of the same execution unit.
        foreach {api key} {
            regular  x
            perkey   expire_x
        } {
            test "Test lazy expire ($api API)" {
                r flushall
                r DEBUG SET-ACTIVE-EXPIRE 0
                set repl [attach_to_replication_stream]

                r set $key 1
                r pexpire $key 1
                after 10
                assert_equal {} [r get $key]

                if {$api eq "regular"} {
                    assert_replication_stream $repl {
                        {select *}
                        {set x 1}
                        {pexpireat x *}
                        {multi}
                        {del x}
                        {lpush before_expired x}
                        {incr expired}
                        {exec}
                    }
                } else {
                    assert_replication_stream $repl {
                        {select *}
                        {set expire_x 1}
                        {pexpireat expire_x *}
                        {multi}
                        {del expire_x}
                        {lpush expired_keys expire_x}
                        {lpush before_expired expire_x}
                        {exec}
                    }
                }
                close_replication_stream $repl
                r DEBUG SET-ACTIVE-EXPIRE 1
            } {OK} {needs:debug}
        }

        # Common: an outer post-notification callback's RM_Call accesses an
        # already-expired sibling key; the resulting lazy DEL fires KSN, which
        # registers another post-notification job from inside the outer
        # callback. That inner job must fire from the outer drain, not nested
        # inside the outer callback's stack. The regular API gates this via
        # execution_nesting; the per-key API gates it via the dedicated
        # firing_keyed_post_notif_jobs flag.
        foreach {api outer_key inner_key} {
            regular  read_x                 x
            perkey   pkread_expire_target   expire_target
        } {
            test "Test lazy expire inside post job notification ($api API)" {
                r flushall
                r DEBUG SET-ACTIVE-EXPIRE 0
                set repl [attach_to_replication_stream]

                r set $inner_key 1
                r pexpire $inner_key 1
                after 10
                assert_equal {OK} [r set $outer_key 1]

                if {$api eq "regular"} {
                    assert_replication_stream $repl {
                        {select *}
                        {set x 1}
                        {pexpireat x *}
                        {multi}
                        {set read_x 1}
                        {del x}
                        {lpush before_expired x}
                        {incr expired}
                        {exec}
                    }
                } else {
                    assert_replication_stream $repl {
                        {select *}
                        {set expire_target 1}
                        {pexpireat expire_target *}
                        {multi}
                        {set pkread_expire_target 1}
                        {del expire_target}
                        {lpush expired_keys expire_target}
                        {lpush before_expired expire_target}
                        {exec}
                    }
                }
                close_replication_stream $repl
                r DEBUG SET-ACTIVE-EXPIRE 1
            } {OK} {needs:debug}
        }

        # Regular-only: tests REDISMODULE_OPTIONS_ALLOW_NESTED_KEYSPACE_NOTIFICATIONS,
        # which is orthogonal to the post-notification API surface.
        test {Test nested keyspace notification} {
            r flushall
            set repl [attach_to_replication_stream]

            assert_equal {OK} [r set write_sync_write_sync_x 1]

            assert_replication_stream $repl {
                {multi}
                {select *}
                {set x 1}
                {set write_sync_x 1}
                {set write_sync_write_sync_x 1}
                {exec}
            }
            close_replication_stream $repl
        }

        # Both APIs accept registrations from a NOTIFY_EVICTED handler
        # because eviction fires inside the OOM-triggering command's call(),
        # so server.executing_client is set. Tested only via the regular
        # fixture for brevity — the per-key path is structurally identical.
        test {Test eviction} {
            r flushall
            set repl [attach_to_replication_stream]
            r set x 1
            r config set maxmemory-policy allkeys-random
            r config set maxmemory 1

            assert_error {OOM *} {r set y 1}

            # the {lpush before_evicted x} is a post notification job registered before 'x' got evicted
            assert_replication_stream $repl {
                {select *}
                {set x 1}
                {multi}
                {del x}
                {lpush before_evicted x}
                {incr evicted}
                {exec}
            }
            r config set maxmemory 0
            close_replication_stream $repl
        } {} {needs:config-maxmemory}

        # Per-key-only: the per-key callback fires at the tail of EVERY call(),
        # including each sub-command inside MULTI/EXEC. The regular API only
        # fires at the outermost EXEC.
        test {Per-key post notification job fires between MULTI/EXEC sub-commands} {
            r flushall
            set repl [attach_to_replication_stream]

            r multi
            r set batched_a 1
            r set batched_b 2
            r set batched_c 3
            r exec

            assert_equal {batched_c batched_b batched_a} [r lrange batched_keys 0 -1]

            assert_replication_stream $repl {
                {multi}
                {select *}
                {set batched_a 1}
                {lpush batched_keys batched_a}
                {set batched_b 2}
                {lpush batched_keys batched_b}
                {set batched_c 3}
                {lpush batched_keys batched_c}
                {exec}
            }
            close_replication_stream $repl
        }

        # Per-key-only: the firing function uses its own reentrance guard
        # (firing_keyed_post_notif_jobs) because per-key callbacks must fire
        # even when execution_nesting > 0. Test that a nested RM_Call inside
        # an outer per-key callback does not re-enter the firing function.
        test {Per-key callback does not re-enter firing while a nested RM_Call is in flight} {
            r flushall

            r set reentrant_outer 1

            set log [r lrange reentrance_log 0 -1]
            assert_equal -1 [lsearch $log "REENTRANCE_DETECTED"]
            assert_equal {inner_after_outer outer_done} $log
        }

        # Per-key-only: the single-key guard refuses registration when the
        # current command touches more than one key. The regular API has no
        # such constraint. We also assert that the refusal logs a warning so
        # module authors get a hint when they hit this.
        test {Per-key post notification job is refused on multi-key commands} {
            r flushall
            set repl [attach_to_replication_stream]
            set baseline [count_log_message 0 "AddPostNotificationJobForKey"]

            r mset batched_a 1 batched_b 2 batched_c 3
            assert_equal {} [r lrange batched_keys 0 -1]

            # MSET touches 3 keys; the keyspace handler fires once per key, so
            # the warning is emitted three times.
            set after [count_log_message 0 "AddPostNotificationJobForKey"]
            assert_equal 3 [expr {$after - $baseline}]

            assert_replication_stream $repl {
                {select *}
                {mset batched_a 1 batched_b 2 batched_c 3}
            }
            close_replication_stream $repl
        }

        # Per-key-only: HSET and HEXPIRE both touch a single hash, so they
        # pass the single-key guard. The per-key callback fires at the tail of
        # each sub-command's call(), interleaving an LPUSH between successive
        # HSET/HEXPIRE on the same hash inside MULTI/EXEC (the original
        # motivation for this API — RED-197766).
        test {Per-key post notification job fires between HSET and HEXPIRE on the same hash inside MULTI/EXEC} {
            r flushall
            set repl [attach_to_replication_stream]

            r multi
            r hset hash_h f1 v1
            r hset hash_h f2 v2
            r hexpire hash_h 100 FIELDS 1 f1
            r exec

            assert_equal {hash_h hash_h hash_h} [r lrange hash_keys 0 -1]

            assert_replication_stream $repl {
                {multi}
                {select *}
                {hset hash_h f1 v1}
                {lpush hash_keys hash_h}
                {hset hash_h f2 v2}
                {lpush hash_keys hash_h}
                {hpexpireat hash_h * FIELDS 1 f1}
                {lpush hash_keys hash_h}
                {exec}
            }
            close_replication_stream $repl
        }

    }
}

set testmodule2 [file normalize tests/modules/keyspace_events.so]

tags "modules external:skip" {
    start_server {} {
        r module load $testmodule with_key_events
        r module load $testmodule2
        test {Test write on post notification callback} {
            set repl [attach_to_replication_stream]

            r set string_x 1
            assert_equal {1} [r get string_changed{string_x}]
            assert_equal {1} [r get string_total]

            r set string_x 2
            assert_equal {2} [r get string_changed{string_x}]
            assert_equal {2} [r get string_total]

            r set string1_x 1
            assert_equal {1} [r get string_changed{string1_x}]
            assert_equal {3} [r get string_total]

            # the {lpush before_overwritten string_x} is a post notification job registered before 'string_x' got overwritten
            assert_replication_stream $repl {
                {multi}
                {select *}
                {set string_x 1}
                {incr string_changed{string_x}}
                {incr string_total}
                {exec}
                {multi}
                {set string_x 2}
                {lpush before_overwritten string_x}
                {incr string_changed{string_x}}
                {incr string_total}
                {exec}
                {multi}
                {set string1_x 1}
                {incr string_changed{string1_x}}
                {incr string_total}
                {exec}
            }
            close_replication_stream $repl
        }
    }
}
