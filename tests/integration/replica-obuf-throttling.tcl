#
# Copyright (c) 2009-Present, Redis Ltd.
# All rights reserved.
#
# Licensed under your choice of (a) the Redis Source Available License 2.0
# (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
# GNU Affero General Public License v3 (AGPLv3).
#

start_server {tags {"repl external:skip"}} {
start_server {} {
start_server {} {
    set replica2 [srv -2 client]
    set replica [srv -1 client]
    set master [srv 0 client]
    set master_host [srv 0 host]
    set master_port [srv 0 port]

    $master config set save ""
    $master config set repl-diskless-sync yes
    $master config set repl-diskless-sync-delay 0
    $master config set repl-rdb-channel no
    # Avoid disconnects while we intentionally grow the replica buffer.
    $master config set client-output-buffer-limit "replica 0 0 0"
    # Low threshold so modest write load during full sync arms throttling.
    $master config set replica-output-buffer-throttling "1024 1kb 30mb 3000"

    test {Replica output buffer throttling postpones clients without blocking replication handshakes} {
        # Slow RDB generation so the replica stays mid-sync (not ONLINE) while
        # the master keeps feeding it the write stream.
        $master config set rdb-key-save-delay 1000000
        populate 50 "" 100
        set existing [redis_deferring_client]
        $existing set before-throttle 1
        assert_equal OK [$existing read]

        set loglines [count_log_lines 0]
        $replica replicaof $master_host $master_port

        wait_for_condition 50 100 {
            ([s 0 rdb_bgsave_in_progress] == 1) &&
            [lindex [$replica role] 3] eq {sync}
        } else {
            fail "replica did not enter full sync"
        }

        # Grow the shared replication buffer without blocking this test client.
        set load [start_write_load $master_host $master_port 10 "" 1024]
        wait_for_log_messages 0 {"*triggered request throttling*"} $loglines 50 100
        stop_write_load $load

        # A new client's first command is allowed immediately.
        set fresh [redis $master_host $master_port 1 $::tls]
        set start [clock milliseconds]
        $fresh set first-command allowed
        assert_equal OK [$fresh read]
        assert_lessthan [expr {[clock milliseconds] - $start}] 1000

        # Later commands from both clients are parsed and postponed. Pipelined
        # commands remain ordered and execute when throttling expires.
        set start [clock milliseconds]
        $existing set buffered-command value
        $existing get buffered-command
        $fresh get first-command

        # A second replica must complete PING/AUTH/REPLCONF/PSYNC while normal
        # client commands are postponed.
        set handshake_start [clock milliseconds]
        set handshake_loglines [count_log_lines 0]
        $replica2 replicaof $master_host $master_port
        wait_for_log_messages 0 {"*asks for synchronization*"} $handshake_loglines 20 100
        set handshake_elapsed [expr {[clock milliseconds] - $handshake_start}]
        assert_lessthan $handshake_elapsed 2500

        assert_equal OK [$existing read]
        assert_equal value [$existing read]
        assert_equal allowed [$fresh read]
        assert_morethan [expr {[clock milliseconds] - $start}] 2000
        $existing close
        $fresh close
    }

    # Unblock the slow child so the suite can shut down cleanly.
    $master config set rdb-key-save-delay 0
    catch {$replica replicaof no one}
    catch {$replica2 replicaof no one}
}
}
}
