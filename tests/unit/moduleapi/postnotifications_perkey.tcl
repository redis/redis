set testmodule [file normalize tests/modules/postnotifications_perkey.so]

tags "modules external:skip" {
    start_server {} {
        r module load $testmodule

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
    }
}
