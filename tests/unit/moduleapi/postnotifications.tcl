set testmodule [file normalize tests/modules/postnotifications.so]

# ----------------------------------------------------------------------------
# The test module accepts a "regular" or "perkey" load arg that selects which
# post-notification API its handlers register against. The handlers' key
# prefixes and post-job side effects are identical in either mode — only the
# RM_AddPostNotificationJob vs RM_AddPostNotificationJobForKey call differs.
# That lets the common tests use identical keys, asserts, and expected
# replication streams for both APIs by wrapping the start_server itself in a
# foreach.
#
# `with_key_events` is loaded *only* by the regular-only block, since the
# server-event LPUSHes (`before_*`) ride the regular drain in both modes but
# their position differs vs. the per-key drain. Server-event interleaving is
# tested explicitly in that block.
# ----------------------------------------------------------------------------

foreach api {regular perkey} {
    tags "modules external:skip" {
        start_server {} {
            r module load $testmodule $api

            test "Test write on post notification callback ($api API)" {
                r flushall
                set repl [attach_to_replication_stream]

                r set string_x 1
                assert_equal {1} [r get string_changed{string_x}]
                assert_equal {1} [r get string_total]

                r set string_x 2
                assert_equal {2} [r get string_changed{string_x}]
                assert_equal {2} [r get string_total]

                assert_replication_stream $repl {
                    {multi}
                    {select *}
                    {set string_x 1}
                    {incr string_changed{string_x}}
                    {incr string_total}
                    {exec}
                    {multi}
                    {set string_x 2}
                    {incr string_changed{string_x}}
                    {incr string_total}
                    {exec}
                }
                close_replication_stream $repl
            }

            test "Test write on post notification callback from module thread ($api API)" {
                r flushall
                set repl [attach_to_replication_stream]

                assert_equal {OK} [r postnotification.async_set string_x]
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
                close_replication_stream $repl
            }

            test "Test lazy expire ($api API)" {
                r flushall
                r DEBUG SET-ACTIVE-EXPIRE 0
                set repl [attach_to_replication_stream]

                r set x 1
                r pexpire x 1
                after 10
                assert_equal {} [r get x]

                assert_replication_stream $repl {
                    {select *}
                    {set x 1}
                    {pexpireat x *}
                    {multi}
                    {del x}
                    {incr expired}
                    {exec}
                }
                close_replication_stream $repl
                r DEBUG SET-ACTIVE-EXPIRE 1
            } {OK} {needs:debug}

            test "Test lazy expire inside post job notification ($api API)" {
                r flushall
                r DEBUG SET-ACTIVE-EXPIRE 0
                set repl [attach_to_replication_stream]

                r set x 1
                r pexpire x 1
                after 10
                assert_equal {OK} [r set read_x 1]

                assert_replication_stream $repl {
                    {select *}
                    {set x 1}
                    {pexpireat x *}
                    {multi}
                    {set read_x 1}
                    {del x}
                    {incr expired}
                    {exec}
                }
                close_replication_stream $repl
                r DEBUG SET-ACTIVE-EXPIRE 1
            } {OK} {needs:debug}
        }
    }
}

# Regular-only tests: behaviors that depend on the regular API specifically
# (server events register via the regular API), or that the per-key API
# refuses outright (active expire fires from cron with executing_client=NULL),
# or that are orthogonal to the post-notification API (nested KSN).
tags "modules external:skip" {
    start_server {} {
        r module load $testmodule regular with_key_events

        test {Server event before_overwritten interleaves with the KSN-driven post-notification job} {
            r flushall
            set repl [attach_to_replication_stream]

            # The first SET creates string_x. The second SET overwrites it,
            # which fires the RedisModuleEvent_Key.before_overwritten server
            # event. That server event registers a post-job (via the regular
            # API) that LPUSHes string_x into `before_overwritten`. Both side
            # effects ride the regular drain — the server-event LPUSH was
            # registered first (inside dbGenericDelete, before
            # notifyKeyspaceEvent), so it appears before the KSN-driven INCRs.
            r set string_x 1
            r set string_x 2

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
            close_replication_stream $repl
        }

        # True API divergence: active expire fires from cron with
        # server.executing_client == NULL, so the per-key API's single-key
        # guard refuses the registration. Regular-only by construction.
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

        # Orthogonal: tests REDISMODULE_OPTIONS_ALLOW_NESTED_KEYSPACE_NOTIFICATIONS,
        # not the post-notification API surface.
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

        # Both APIs would accept (eviction fires inside the OOM-triggering
        # command's call(), so executing_client is set), but the per-key path
        # is structurally identical and only the regular fixture is exercised
        # for brevity. Loaded with with_key_events to also assert the
        # before_evicted server event.
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
    }
}

# Per-key-only tests: behaviors with no regular-API equivalent (the per-key
# API fires at the tail of every call() including each MULTI/EXEC
# sub-command, uses a dedicated reentrance guard, refuses multi-key
# commands, and is the original motivation for HSET+HEXPIRE interleaving).
tags "modules external:skip" {
    start_server {} {
        r module load $testmodule perkey

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

        test {Per-key callback does not re-enter firing while a nested RM_Call is in flight} {
            r flushall

            r set reentrant_outer 1

            set log [r lrange reentrance_log 0 -1]
            assert_equal -1 [lsearch $log "REENTRANCE_DETECTED"]
            assert_equal {inner_after_outer outer_done} $log
        }

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
        r module load $testmodule regular with_key_events
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
