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
    $master config set replica-output-buffer-throttling "1024 1mb 30mb 200"

    test {Replica output buffer throttling triggers during full sync} {
        # Slow RDB generation so the replica stays mid-sync (not ONLINE) while
        # the master keeps feeding it the write stream.
        $master config set rdb-key-save-delay 1000000
        populate 50 "" 100

        set loglines [count_log_lines 0]
        $replica replicaof $master_host $master_port

        wait_for_condition 50 100 {
            ([s 0 rdb_bgsave_in_progress] == 1) &&
            [lindex [$replica role] 3] eq {sync}
        } else {
            fail "replica did not enter full sync"
        }

        # Grow the shared replication buffer while the replica is still syncing.
        populate 2048 "" 1024

        wait_for_log_messages 0 {"*triggered request throttling*"} $loglines 50 100

        # Existing clients should still recover after the short pause.
        assert_equal PONG [$master ping]
    }

    # Unblock the slow child so the suite can shut down cleanly.
    $master config set rdb-key-save-delay 0
    catch {$replica replicaof no one}
}
}
