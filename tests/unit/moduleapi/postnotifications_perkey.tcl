set testmodule [file normalize tests/modules/postnotifications_perkey.so]

tags "modules external:skip" {
    start_server {} {
        r module load $testmodule

        test {Per-key post notification job fires on a single command outside MULTI} {
            r flushall
            set repl [attach_to_replication_stream]

            # The simplest firing path: one SET outside any MULTI/EXEC. The
            # per-key callback fires at the tail of SET's call() and its
            # LPUSH side-effect propagates wrapped with the SET in one
            # implicit MULTI/EXEC.
            r set batched_a 1
            assert_equal {batched_a} [r lrange batched_keys 0 -1]

            assert_replication_stream $repl {
                {multi}
                {select *}
                {set batched_a 1}
                {lpush batched_keys batched_a}
                {exec}
            }
            close_replication_stream $repl
        }

        test {Test per-key post notification job fires between MULTI/EXEC sub-commands} {
            r flushall
            set repl [attach_to_replication_stream]

            r multi
            r set batched_a 1
            r set batched_b 2
            r set batched_c 3
            r exec

            # Each SET's keyed callback fires at the tail of its own sub-command,
            # before the next sub-command runs, so LPUSHes are interleaved with
            # the SETs inside the MULTI/EXEC propagation block.
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

            # SET reentrant_outer triggers a keyed-job registration.
            # That callback sets a marker and issues an internal SET on
            # reentrant_inner, which registers another keyed job. If the
            # firing function re-entered while still inside the outer
            # callback, the inner branch would observe the marker set and
            # log REENTRANCE_DETECTED. With the guard, the inner job is
            # picked up by the outer drain only after the outer callback
            # has returned and the marker has been cleared.
            r set reentrant_outer 1

            set log [r lrange reentrance_log 0 -1]
            assert_equal -1 [lsearch $log "REENTRANCE_DETECTED"]
            assert_equal {inner_after_outer outer_done} $log
        }

        test {Per-key post notification job is refused on multi-key commands} {
            r flushall
            set repl [attach_to_replication_stream]

            # MSET touches multiple keys; AddPostNotificationJobForKey must refuse
            # the registration from KSN, so no LPUSH side-effect is propagated.
            r mset batched_a 1 batched_b 2 batched_c 3
            assert_equal {} [r lrange batched_keys 0 -1]

            assert_replication_stream $repl {
                {select *}
                {mset batched_a 1 batched_b 2 batched_c 3}
            }
            close_replication_stream $repl
        }

        test {Per-key post notification job fires between HSET and HEXPIRE on the same hash inside MULTI/EXEC} {
            r flushall
            set repl [attach_to_replication_stream]

            # HSET and HEXPIRE both touch exactly one key, so they pass the
            # single-key guard. The per-key callback fires at the tail of each
            # sub-command's call(), interleaving an LPUSH side-effect between
            # successive HSET/HEXPIRE on the same hash. This is the original
            # motivation for the per-key API (RED-197766).
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

        test {Lazy expire registers a per-key post notification job} {
            r flushall
            r DEBUG SET-ACTIVE-EXPIRE 0
            set repl [attach_to_replication_stream]

            # GET on an expired key triggers a lazy DEL inside GET's call().
            # The DEL fires NOTIFY_EXPIRED with server.executing_client still
            # set to the GET, so the single-key guard accepts the registration
            # and the per-key callback LPUSHes the expired key to the sink.
            r set expire_x 1
            r pexpire expire_x 1
            after 10
            assert_equal {} [r get expire_x]

            assert_equal {expire_x} [r lrange expired_keys 0 -1]

            assert_replication_stream $repl {
                {select *}
                {set expire_x 1}
                {pexpireat expire_x *}
                {multi}
                {del expire_x}
                {lpush expired_keys expire_x}
                {exec}
            }
            close_replication_stream $repl
            r DEBUG SET-ACTIVE-EXPIRE 1
        } {OK} {needs:debug}

        test {Lazy expire from inside an outer per-key callback registers a second per-key job} {
            r flushall
            r DEBUG SET-ACTIVE-EXPIRE 0
            set repl [attach_to_replication_stream]

            # Combined coverage of the reentrance guard and lazy-expire-driven
            # KSN: SET read_expire_target's per-key callback issues GET
            # expire_target, whose lazy DEL fires NOTIFY_EXPIRED and registers
            # a second per-key job from inside the outer callback. That job
            # must fire from the outer drain (not nested inside the outer
            # callback's stack), which is what the reentrance guard combined
            # with the per-call() firing hook makes possible.
            r set expire_target 1
            r pexpire expire_target 1
            after 10
            r set read_expire_target 1

            assert_equal {expire_target} [r lrange expired_keys 0 -1]

            assert_replication_stream $repl {
                {select *}
                {set expire_target 1}
                {pexpireat expire_target *}
                {multi}
                {set read_expire_target 1}
                {del expire_target}
                {lpush expired_keys expire_target}
                {exec}
            }
            close_replication_stream $repl
            r DEBUG SET-ACTIVE-EXPIRE 1
        } {OK} {needs:debug}
    }
}
