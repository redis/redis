set testmodule [file normalize tests/modules/postnotifications.so]

foreach api {regular perkey} {
    tags "modules external:skip" {
        start_server {} {
            r module load $testmodule $api

            test "Test write on post notification callback ($api API)" {
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
        }
    }
}

foreach api {regular perkey} {
    tags "modules external:skip" {
        start_server {} {
            r module load $testmodule $api

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
        }
    }
}

foreach api {regular perkey} {
    tags "modules external:skip" {
        start_server {} {
            r module load $testmodule $api with_key_events

            test "Test active expire ($api API)" {
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

                # {lpush before_expired x} comes from the RedisModuleEvent_Key
                # server event (always uses the regular post-notif queue).
                # {incr expired} comes from the keyspace handler (regular or
                # per-key queue depending on $api). Both APIs propagate the
                # same stream: postExecutionUnitOperations drains regular
                # before per-key, so the ordering between the two side-effects
                # matches their in-process registration order.
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
        }
    }
}

foreach api {regular perkey} {
    tags "modules external:skip" {
        start_server {} {
            r module load $testmodule $api

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
        }
    }
}

foreach api {regular perkey} {
    tags "modules external:skip" {
        start_server {} {
            r module load $testmodule $api

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

tags "modules external:skip" {
    start_server {} {
        r module load $testmodule with_key_events

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
    }
}

foreach api {regular perkey} {
    tags "modules external:skip" {
        start_server {} {
            r module load $testmodule $api with_key_events

            test "Test eviction ($api API)" {
                r flushall
                set repl [attach_to_replication_stream]
                r set x 1
                r config set maxmemory-policy allkeys-random
                r config set maxmemory 1

                assert_error {OOM *} {r set y 1}

                # {lpush before_evicted x} comes from the
                # RedisModuleEvent_Key/before_evicted server event (always uses
                # the regular post-notif queue). {incr evicted} comes from the
                # keyspace handler (regular or per-key queue depending on
                # $api). Both APIs propagate the same stream: regular drains
                # before per-key inside postExecutionUnitOperations.
                assert_replication_stream $repl {
                    {select *}
                    {set x 1}
                    {multi}
                    {del x}
                    {lpush before_evicted x}
                    {incr evicted}
                    {exec}
                }
                close_replication_stream $repl
            } {} {needs:config-maxmemory}
        }
    }
}

# Per-key-only tests (no regular-API equivalent).
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

        test {Per-key post notification job fires per affected key on multi-key commands} {
            r flushall
            set repl [attach_to_replication_stream]

            # MSET emits one notifyKeyspaceEvent per key. Each dispatch sets
            # server.in_keyspace_notification, so the keyspace handler can
            # register one per-key job per affected key. All three jobs fire
            # at the tail of MSET's call() and propagate inside the same
            # multi/exec the propagation buffer flushes.
            r mset batched_a 1 batched_b 2 batched_c 3
            assert_equal {batched_c batched_b batched_a} [r lrange batched_keys 0 -1]

            assert_replication_stream $repl {
                {multi}
                {select *}
                {mset batched_a 1 batched_b 2 batched_c 3}
                {lpush batched_keys batched_a}
                {lpush batched_keys batched_b}
                {lpush batched_keys batched_c}
                {exec}
            }
            close_replication_stream $repl
        }

        test {Per-key post notification job fires per missing key on MGET (multi-key read)} {
            r flushall
            set repl [attach_to_replication_stream]

            # MGET emits one NOTIFY_KEY_MISS notification per missing key. Each
            # dispatch sets server.in_keyspace_notification, so the per-key
            # handler registers one job per miss. The jobs drain at the tail of
            # MGET's call(), propagating as a multi/exec after the read.
            assert_equal {{} {} {}} [r mget miss_a miss_b miss_c]
            assert_equal {miss_c miss_b miss_a} [r lrange mget_misses 0 -1]

            assert_replication_stream $repl {
                {multi}
                {select *}
                {lpush mget_misses miss_a}
                {lpush mget_misses miss_b}
                {lpush mget_misses miss_c}
                {exec}
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
            }
            close_replication_stream $repl
        }
    }
}
