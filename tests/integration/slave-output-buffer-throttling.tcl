#
# Copyright (c) 2021-Present, Redis Ltd.
# All rights reserved.
#
# Licensed under your choice of (a) the Redis Source Available License 2.0
# (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
# GNU Affero General Public License v3 (AGPLv3).
#

# These tests cover the "slave-output-buffer-throttling" feature: while a
# replica's main replication channel is still mid-full-sync (not yet online)
# and its output buffer grows past a configured threshold (e.g. because the
# replica stopped reading), the master throttles processing of new client
# requests for a short, bounded amount of time via the request throttler
# (src/request_throttler.c), instead of unconditionally disconnecting the
# replica or letting its output buffer grow unbounded.
start_server {tags {"repl external:skip"}} {
    start_server {} {
        set replica [srv -1 client]
        set replica_host [srv -1 host]
        set replica_port [srv -1 port]
        set master [srv 0 client]
        set master_host [srv 0 host]
        set master_port [srv 0 port]

        $master config set save ""
        $master config set repl-diskless-sync yes
        $master config set repl-diskless-sync-delay 0
        $master config set repl-rdb-channel yes
        # Disable the regular hard/soft output buffer limit so that it
        # doesn't disconnect the replica before we can observe the throttle.
        $master config set client-output-buffer-limit "replica 0 0 0"
        $replica config set repl-rdb-channel yes

        # Slow down RDB generation so the replica's main channel connection
        # stays in the pre-online "send_bulk_and_stream" state for a while,
        # giving us a window in which a paused replica's un-drained output
        # buffer can be observed growing past the throttle threshold.
        $master config set rdb-key-save-delay 100000
        populate 200 "" 2000

        # Grow the (paused, un-drained) replica's main-channel output buffer
        # well past the configured threshold, while it's still mid-sync, with
        # a single large write. The kernel's TCP receive buffer on the
        # (paused) replica side can auto-tune up to several tens of MB and
        # silently absorb smaller/many writes without the master ever seeing
        # backpressure, so a large single value (tens of MB) is used to
        # reliably force Redis's own per-replica output buffer to grow.
        # Using one write (instead of many smaller ones) also avoids the
        # throttle - once triggered - delaying this same connection's own
        # subsequent writes: the throttle only blocks *reading new commands*,
        # and it's only armed once this command finishes and propagates.
        proc grow_paused_replica_output_buffer {master} {
            $master set trigger-key [string repeat z 64000000]
        }

        test {Slave output buffer throttling delays new requests while a replica is stuck mid-sync} {
            $master config set slave-output-buffer-throttling "500000 500000 1000000000 3000"

            $replica replicaof $master_host $master_port
            after 200
            set replica_pid [s -1 process_id]
            pause_process $replica_pid

            set err [catch {
                grow_paused_replica_output_buffer $master

                # redis_deferring_client already issues a SELECT (or, in
                # singledb mode, a PING) while connecting, which is enough to
                # set lastcmd and make it a non-exempt "existing connection"
                # for the throttler - so the very next command below is the
                # one that should observe the delay. Do not add another
                # priming command here, or that one will absorb the delay
                # instead of the timed one.
                set rd [redis_deferring_client]

                set start_time [clock milliseconds]
                $rd ping
                set reply [$rd read]
                set elapsed [expr {[clock milliseconds]-$start_time}]

                assert_equal "PONG" $reply
                # max delay is 3000ms: the request must have been delayed
                # close to that, but the server must not be wedged forever.
                assert {$elapsed >= 1500 && $elapsed <= 8000}

                # The throttle window has elapsed: subsequent requests must
                # go back to being served promptly.
                set start_time [clock milliseconds]
                $rd ping
                set reply [$rd read]
                set elapsed [expr {[clock milliseconds]-$start_time}]

                assert_equal "PONG" $reply
                assert {$elapsed < 500}

                $rd close
            } msg]

            resume_process $replica_pid
            $replica replicaof no one
            if {$err} { error $msg }
        }

        test {Slave output buffer throttling is a no-op when disabled (the default)} {
            $master config set slave-output-buffer-throttling "0 0 0 0"

            $replica replicaof $master_host $master_port
            after 200
            set replica_pid [s -1 process_id]
            pause_process $replica_pid

            set err [catch {
                grow_paused_replica_output_buffer $master

                # redis_deferring_client already issues a SELECT (or, in
                # singledb mode, a PING) while connecting, which is enough to
                # set lastcmd and make it a non-exempt "existing connection"
                # for the throttler - so the very next command below is the
                # one that should observe the delay. Do not add another
                # priming command here, or that one will absorb the delay
                # instead of the timed one.
                set rd [redis_deferring_client]

                set start_time [clock milliseconds]
                $rd ping
                set reply [$rd read]
                set elapsed [expr {[clock milliseconds]-$start_time}]

                assert_equal "PONG" $reply
                assert {$elapsed < 500}

                $rd close
            } msg]

            resume_process $replica_pid
            if {$err} { error $msg }
        }
    }
}
