set testmodule [file normalize tests/modules/blockedclient.so]
set testmodule2 [file normalize tests/modules/postnotifications.so]
set testmodule3 [file normalize tests/modules/blockonkeys.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test {Locked GIL acquisition from async RM_Call} {
        assert_equal {OK} [r do_rm_call_async acquire_gil]
    }

    test "Blpop on async RM_Call fire and forget" {
        assert_equal {Blocked} [r do_rm_call_fire_and_forget blpop l 0]
        r lpush l a
        assert_equal {0} [r llen l]
    }

    test "Blpop on threaded async RM_Call" {
        set rd [redis_deferring_client]

        $rd do_rm_call_async_on_thread blpop l 0
        wait_for_blocked_clients_count 1
        r lpush l a
        assert_equal [$rd read] {l a}
        wait_for_blocked_clients_count 0
        $rd close
    }

    foreach cmd {do_rm_call_async do_rm_call_async_script_mode } {

        test "Blpop on async RM_Call using $cmd" {
            set rd [redis_deferring_client]

            $rd $cmd blpop l 0
            wait_for_blocked_clients_count 1
            r lpush l a
            assert_equal [$rd read] {l a}
            wait_for_blocked_clients_count 0
            $rd close
        }

        test "Brpop on async RM_Call using $cmd" {
            set rd [redis_deferring_client]

            $rd $cmd brpop l 0
            wait_for_blocked_clients_count 1
            r lpush l a
            assert_equal [$rd read] {l a}
            wait_for_blocked_clients_count 0
            $rd close
        }

        test "Brpoplpush on async RM_Call using $cmd" {
            set rd [redis_deferring_client]

            $rd $cmd brpoplpush l1 l2 0
            wait_for_blocked_clients_count 1
            r lpush l1 a
            assert_equal [$rd read] {a}
            wait_for_blocked_clients_count 0
            $rd close
            r lpop l2
        } {a}

        test "Blmove on async RM_Call using $cmd" {
            set rd [redis_deferring_client]

            $rd $cmd blmove l1 l2 LEFT LEFT 0
            wait_for_blocked_clients_count 1
            r lpush l1 a
            assert_equal [$rd read] {a}
            wait_for_blocked_clients_count 0
            $rd close
            r lpop l2
        } {a}

        test "Bzpopmin on async RM_Call using $cmd" {
            set rd [redis_deferring_client]

            $rd $cmd bzpopmin s 0
            wait_for_blocked_clients_count 1
            r zadd s 10 foo
            assert_equal [$rd read] {s foo 10}
            wait_for_blocked_clients_count 0
            $rd close
        }

        test "Bzpopmax on async RM_Call using $cmd" {
            set rd [redis_deferring_client]

            $rd $cmd bzpopmax s 0
            wait_for_blocked_clients_count 1
            r zadd s 10 foo
            assert_equal [$rd read] {s foo 10}
            wait_for_blocked_clients_count 0
            $rd close
        }
    }

    test {Nested async RM_Call} {
        set rd [redis_deferring_client]

        $rd do_rm_call_async do_rm_call_async do_rm_call_async do_rm_call_async blpop l 0
        wait_for_blocked_clients_count 1
        r lpush l a
        assert_equal [$rd read] {l a}
        wait_for_blocked_clients_count 0
        $rd close
    }

    test {Test multiple async RM_Call waiting on the same event} {
        set rd1 [redis_deferring_client]
        set rd2 [redis_deferring_client]

        $rd1 do_rm_call_async do_rm_call_async do_rm_call_async do_rm_call_async blpop l 0
        $rd2 do_rm_call_async do_rm_call_async do_rm_call_async do_rm_call_async blpop l 0
        wait_for_blocked_clients_count 2
        r lpush l element element
        assert_equal [$rd1 read] {l element}
        assert_equal [$rd2 read] {l element}
        wait_for_blocked_clients_count 0
        $rd1 close
        $rd2 close
    }

    test {async RM_Call calls RM_Call} {
        assert_equal {PONG} [r do_rm_call_async do_rm_call ping]
    }

    test {async RM_Call calls background RM_Call calls RM_Call} {
        assert_equal {PONG} [r do_rm_call_async do_bg_rm_call do_rm_call ping]
    }

    test {async RM_Call calls background RM_Call calls RM_Call calls async RM_Call} {
        assert_equal {PONG} [r do_rm_call_async do_bg_rm_call do_rm_call do_rm_call_async ping]
    }

    test {async RM_Call inside async RM_Call callback} {
        set rd [redis_deferring_client]
        $rd wait_and_do_rm_call blpop l 0
        wait_for_blocked_clients_count 1

        start_server {} {
            test "Connect a replica to the master instance" {
                r slaveof [srv -1 host] [srv -1 port]
                wait_for_condition 50 100 {
                    [s role] eq {slave} &&
                    [string match {*master_link_status:up*} [r info replication]]
                } else {
                    fail "Can't turn the instance into a replica"
                }
            }

            assert_equal {1} [r -1 lpush l a]
            assert_equal [$rd read] {l a}
        }

        wait_for_blocked_clients_count 0
        $rd close
    }

    test {Become replica while having async RM_Call running} {
        r flushall
        set rd [redis_deferring_client]
        $rd do_rm_call_async blpop l 0
        wait_for_blocked_clients_count 1

        #become a replica of a not existing redis
        r replicaof localhost 30000

        catch {[$rd read]} e
        assert_match {UNBLOCKED force unblock from blocking operation*} $e
        wait_for_blocked_clients_count 0

        r replicaof no one

        r lpush l 1
        # make sure the async rm_call was aborted
        assert_equal [r llen l] {1}
        $rd close
    }

    test {Pipeline with blocking RM_Call} {
        r flushall
        set rd [redis_deferring_client]
        set buf ""
        append buf "do_rm_call_async blpop l 0\r\n"
        append buf "ping\r\n"
        $rd write $buf
        $rd flush
        wait_for_blocked_clients_count 1

        # release the blocked client
        r lpush l 1

        assert_equal [$rd read] {l 1}
        assert_equal [$rd read] {PONG}

        wait_for_blocked_clients_count 0
        $rd close
    }

    test {blocking RM_Call abort} {
        r flushall
        set rd [redis_deferring_client]
        
        $rd client id
        set client_id [$rd read]

        $rd do_rm_call_async blpop l 0
        wait_for_blocked_clients_count 1

        r client kill ID $client_id
        assert_error {*error reading reply*} {$rd read}

        wait_for_blocked_clients_count 0

        r lpush l 1
        # make sure the async rm_call was aborted
        assert_equal [r llen l] {1}
        $rd close
    }
}

start_server {tags {"modules"}} {
    r module load $testmodule

    test {Test basic replication stream on unblock handler} {
        r flushall
        set repl [attach_to_replication_stream]

        set rd [redis_deferring_client]

        $rd do_rm_call_async blpop l 0
        wait_for_blocked_clients_count 1
        r lpush l a
        assert_equal [$rd read] {l a}

        assert_replication_stream $repl {
            {select *}
            {lpush l a}
            {lpop l}
        }
        close_replication_stream $repl

        wait_for_blocked_clients_count 0
        $rd close
    }

    test {Test unblock handler are executed as a unit} {
        r flushall
        set repl [attach_to_replication_stream]

        set rd [redis_deferring_client]

        $rd blpop_and_set_multiple_keys l x 1 y 2
        wait_for_blocked_clients_count 1
        r lpush l a
        assert_equal [$rd read] {OK}

        assert_replication_stream $repl {
            {select *}
            {lpush l a}
            {multi}
            {lpop l}
            {set x 1}
            {set y 2}
            {exec}
        }
        close_replication_stream $repl

        wait_for_blocked_clients_count 0
        $rd close
    }

    test {Test no propagation of blocking command} {
        r flushall
        set repl [attach_to_replication_stream]

        set rd [redis_deferring_client]

        $rd do_rm_call_async_no_replicate blpop l 0
        wait_for_blocked_clients_count 1
        r lpush l a
        assert_equal [$rd read] {l a}

        # make sure the lpop are not replicated
        r set x 1

        assert_replication_stream $repl {
            {select *}
            {lpush l a}
            {set x 1}
        }
        close_replication_stream $repl

        wait_for_blocked_clients_count 0
        $rd close
    }
}

start_server {tags {"modules"}} {
    r module load $testmodule
    r module load $testmodule2

    test {Test unblock handler are executed as a unit with key space notifications} {
        r flushall
        set repl [attach_to_replication_stream]

        set rd [redis_deferring_client]

        $rd blpop_and_set_multiple_keys l string_foo 1 string_bar 2
        wait_for_blocked_clients_count 1
        r lpush l a
        assert_equal [$rd read] {OK}

        assert_replication_stream $repl {
            {select *}
            {lpush l a}
            {multi}
            {lpop l}
            {set string_foo 1}
            {set string_bar 2}
            {incr string_changed{string_foo}}
            {incr string_changed{string_bar}}
            {incr string_total}
            {incr string_total}
            {exec}
        }
        close_replication_stream $repl

        wait_for_blocked_clients_count 0
        $rd close
    }

    test {Test unblock handler are executed as a unit with lazy expire} {
        r flushall
        r DEBUG SET-ACTIVE-EXPIRE 0
        set repl [attach_to_replication_stream]

        set rd [redis_deferring_client]

        $rd blpop_and_set_multiple_keys l string_foo 1 string_bar 2
        wait_for_blocked_clients_count 1
        r lpush l a
        assert_equal [$rd read] {OK}

        # set expiration on string_foo
        r pexpire string_foo 1
        after 10

        # now the key should have been expired
        $rd blpop_and_set_multiple_keys l string_foo 1 string_bar 2
        wait_for_blocked_clients_count 1
        r lpush l a
        assert_equal [$rd read] {OK}

        assert_replication_stream $repl {
            {select *}
            {lpush l a}
            {multi}
            {lpop l}
            {set string_foo 1}
            {set string_bar 2}
            {incr string_changed{string_foo}}
            {incr string_changed{string_bar}}
            {incr string_total}
            {incr string_total}
            {exec}
            {pexpireat string_foo *}
            {lpush l a}
            {multi}
            {lpop l}
            {del string_foo}
            {set string_foo 1}
            {set string_bar 2}
            {incr expired}
            {incr string_changed{string_foo}}
            {incr string_changed{string_bar}}
            {incr string_total}
            {incr string_total}
            {exec}
        }
        close_replication_stream $repl
        r DEBUG SET-ACTIVE-EXPIRE 1
        
        wait_for_blocked_clients_count 0
        $rd close
    }
}

start_server {tags {"modules"}} {
    r module load $testmodule
    r module load $testmodule3

    test {Test unblock handler on module blocked on keys} {
        set rd [redis_deferring_client]

        r fsl.push l 1
        $rd do_rm_call_async FSL.BPOPGT l 3 0
        wait_for_blocked_clients_count 1
        r fsl.push l 2
        r fsl.push l 3
        r fsl.push l 4
        assert_equal [$rd read] {4}

        wait_for_blocked_clients_count 0
        $rd close
    }
}
