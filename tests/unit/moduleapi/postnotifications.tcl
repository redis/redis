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

tags "modules external:skip" {
    start_server {} {
        r module load $testmodule with_key_events

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

                # the {lpush before_evicted x} is a post notification job
                # registered before 'x' got evicted via the RedisModuleEvent_Key
                # server event (always uses the regular post-notif queue).
                #
                # Under the per-key API the keyspace-notification side-effect
                # ({incr evicted}) drains from firePostKeyedNotificationJobs at
                # the top of afterCommand, before the regular post-notif queue
                # drains and before propagatePendingCommands flushes the outer
                # multi/exec. With maxmemory=1 still in force, that per-key
                # call() hits OOM and does not propagate; the trailing
                # {del before_evicted} is the eviction of the list created by
                # the regular drain's lpush.
                if {$api eq "perkey"} {
                    assert_replication_stream $repl {
                        {select *}
                        {set x 1}
                        {multi}
                        {del x}
                        {lpush before_evicted x}
                        {exec}
                        {del before_evicted}
                    }
                } else {
                    assert_replication_stream $repl {
                        {select *}
                        {set x 1}
                        {multi}
                        {del x}
                        {lpush before_evicted x}
                        {incr evicted}
                        {exec}
                    }
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
