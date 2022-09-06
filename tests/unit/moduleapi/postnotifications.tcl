set testmodule [file normalize tests/modules/postnotifications.so]

tags "modules" {
    start_server [list overrides [list loadmodule "$testmodule"]] {

        test {Test write on post notification callback} {
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

        test {Test write on post notification callback from module thread} {
            r flushall
            set repl [attach_to_replication_stream]

            assert_equal {OK} [r postnotification.async_set]
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
        }

        test {Test lazy expire} {
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

        test {Test eviction} {
            r flushall
            set repl [attach_to_replication_stream]
            r set x 1
            r config set maxmemory-policy allkeys-random 
            r config set maxmemory 1

            assert_error {OOM *} {r set y 1}

            assert_replication_stream $repl {
                {select *}
                {set x 1}
                {multi}
                {del x}
                {incr evicted}
                {exec}
            }
            close_replication_stream $repl
        } {} {needs:config-maxmemory}
    }
}