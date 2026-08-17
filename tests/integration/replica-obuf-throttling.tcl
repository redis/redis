#
# Copyright (c) 2009-Present, Redis Ltd.
# All rights reserved.
#
# Licensed under your choice of (a) the Redis Source Available License 2.0
# (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
# GNU Affero General Public License v3 (AGPLv3).
#

# Arm throttling: slow full sync + write load until the master logs a trigger.
proc arm_replica_request_throttling {master replica master_host master_port} {
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

    set load [start_write_load $master_host $master_port 10 "" 1024]
    wait_for_log_messages 0 {"*triggered request throttling*"} $loglines 50 100
    stop_write_load $load
}

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
        populate 50 "" 100
        set existing [redis_deferring_client]
        $existing set before-throttle 1
        assert_equal OK [$existing read]

        arm_replica_request_throttling $master $replica $master_host $master_port

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
    $master config set client-output-buffer-limit "replica 0 0 0"
    $master config set replica-output-buffer-throttling "1024 1kb 30mb 3000"

    test {Unknown command must not reopen first-command throttle bypass} {
        # Existing client (already ran a command) sends an unknown command,
        # which used to clear lastcmd and let the next write skip throttling.
        set rd [redis_deferring_client]
        $rd set before-throttle 1
        assert_equal OK [$rd read]

        arm_replica_request_throttling $master $replica $master_host $master_port

        $rd nosuchcommand
        assert_error {*unknown command*} {$rd read}

        # Do not poll INFO via the test client here: during throttling that
        # client is postponed too. Use elapsed time like the handshake test.
        set start [clock milliseconds]
        $rd set should-be-postponed 1
        assert_equal OK [$rd read]
        assert_morethan [expr {[clock milliseconds] - $start}] 2000
        $rd close
    }

    # Unblock the slow child so the suite can shut down cleanly.
    # Note: a forked RDB child keeps the old rdb-key-save-delay; shutting down
    # this server kills it. Do not wait for bgsave here.
    $master config set rdb-key-save-delay 0
    catch {$replica replicaof no one}
}
}

# Overflow of elapsed*budget only fits in a 32-bit unsigned long. On
# 64-bit the same config never wraps, so skip there.
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
    $master config set client-output-buffer-limit "replica 0 0 0"

    if {[s arch_bits] == 32} {
        test {32-bit throttle budget math must not wrap and falsely arm} {
            # limit=2gb and estimated_repl_time=1 make obuf_bytes_per_sec=2gb.
            # After a couple of seconds, elapsed*rate is exactly 2^32 with
            # 32-bit unsigned long (wraps to 0) but stays correct as ull.
            $master config set replica-output-buffer-throttling "1024 2gb 1mb 3000"
            $master config set rdb-key-save-delay 1000000
            populate 50 "" 100

            set triggers_before [count_log_message 0 "*triggered request throttling*"]
            $replica replicaof $master_host $master_port
            wait_for_condition 50 100 {
                ([s 0 rdb_bgsave_in_progress] == 1) &&
                [lindex [$replica role] 3] eq {sync}
            } else {
                fail "replica did not enter full sync"
            }

            set load [start_write_load $master_host $master_port 10 "" 1024]
            after 2500
            stop_write_load $load

            assert_equal $triggers_before \
                [count_log_message 0 "*triggered request throttling*"]
        }
    }

    $master config set rdb-key-save-delay 0
    catch {$replica replicaof no one}
}
}

# Budget must not grow without bound while stuck in WAIT_BGSAVE: after a long
# stall, uncapped elapsed*rate can exceed a modest backlog and skip throttling.
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
    $master config set client-output-buffer-limit "replica 0 0 0"
    # limit=1mb: after ~4s without a cap, budget ~= 4mb and a 2mb backlog
    # would be forgiven. With the cap, used_mem > 1mb must still arm.
    $master config set replica-output-buffer-throttling "1024 1mb 30mb 3000"

    test {Throttle budget capped so long BGSAVE wait cannot skip arming} {
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

        # Let elapsed_repl_time grow while the replica sits in wait_bgsave.
        # Uncapped budget after 4s is about 4mb; the cap keeps it at 1mb.
        after 4000

        # Pipeline ~2mb of writes on a deferred client (do not wait for replies:
        # once throttling arms that client is postponed). Above the 1mb cap,
        # below an uncapped 4mb budget, so only capped math arms throttling.
        set rd [redis_deferring_client]
        for {set i 0} {$i < 2000} {incr i} {
            $rd set "bgsave-budget:$i" [string repeat x 1024]
        }
        wait_for_log_messages 0 {"*triggered request throttling*"} $loglines 50 100
        $rd close
    }

    $master config set rdb-key-save-delay 0
    catch {$replica replicaof no one}
}
}

# Disabling the feature mid-pause must wake up postponed clients right away,
# not leave them waiting for a stale pause timer to expire on its own.
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
    $master config set client-output-buffer-limit "replica 0 0 0"
    # A large max-delay so the pause would otherwise last far longer than
    # this test is willing to wait, proving the unblock came from CONFIG SET.
    $master config set replica-output-buffer-throttling "1024 1kb 30mb 60000"

    test {CONFIG SET disabling throttling unblocks already-postponed clients} {
        set rd [redis_deferring_client]
        $rd set before-throttle 1
        assert_equal OK [$rd read]

        arm_replica_request_throttling $master $replica $master_host $master_port

        set start [clock milliseconds]
        $rd set should-unblock-on-disable 1

        # Give the postponed command time to actually be queued before we
        # disable the feature. Use a fresh connection for the CONFIG SET:
        # $master already ran commands earlier in this test, so on its
        # connection CONFIG SET would itself be postponed (only a
        # connection's first command is exempt from throttling).
        after 200
        set admin [redis $master_host $master_port 0 $::tls]
        $admin config set replica-output-buffer-throttling "0 0 0 0"
        $admin close

        assert_equal OK [$rd read]
        assert_lessthan [expr {[clock milliseconds] - $start}] 5000
        $rd close
    }

    $master config set rdb-key-save-delay 0
    catch {$replica replicaof no one}
}
}

# obuf_bytes_per_sec = limit / estimated_repl_time uses integer division and
# used to floor to 0 (skipping throttling) whenever estimated_repl_time is
# larger than limit's raw numeric value. This is exactly the slow-sync case
# throttling exists to catch, so it must still arm.
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
    $master config set client-output-buffer-limit "replica 0 0 0"
    # repl-rate=1 byte/sec makes estimated_repl_time astronomically large
    # (server memory in bytes / 1), so limit (1kb) / estimated_repl_time
    # floors to 0 without the fix.
    $master config set replica-output-buffer-throttling "1024 1kb 1 3000"

    test {Throttle must still arm when estimated repl time dwarfs the limit} {
        arm_replica_request_throttling $master $replica $master_host $master_port
    }

    $master config set rdb-key-save-delay 0
    catch {$replica replicaof no one}
}
}

# If the replica that armed throttling disconnects mid-sync, postponed
# clients must not be stuck waiting for a pause meant to protect a replica
# that is already gone.
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
    $master config set client-output-buffer-limit "replica 0 0 0"
    # A large max-delay so the pause would otherwise last far longer than
    # this test is willing to wait, proving the unblock came from the
    # replica disconnecting rather than a natural timeout.
    $master config set replica-output-buffer-throttling "1024 1kb 30mb 60000"

    test {Replica disconnect during full sync unblocks postponed clients} {
        set rd [redis_deferring_client]
        $rd set before-throttle 1
        assert_equal OK [$rd read]

        arm_replica_request_throttling $master $replica $master_host $master_port

        set start [clock milliseconds]
        $rd set should-unblock-on-disconnect 1

        # Give the postponed command time to actually be queued, then drop
        # the syncing replica's connection to the master.
        after 200
        catch {$replica replicaof no one}

        assert_equal OK [$rd read]
        assert_lessthan [expr {[clock milliseconds] - $start}] 5000
        $rd close
    }

    $master config set rdb-key-save-delay 0
}
}

# If the replica that armed throttling finishes catching up normally (goes
# ONLINE) instead of disconnecting, postponed clients must not be stuck
# waiting for a pause meant to protect a sync that has already finished.
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
    $master config set client-output-buffer-limit "replica 0 0 0"
    # A large max-delay so the pause would otherwise last far longer than
    # this test is willing to wait, proving the unblock came from the sync
    # finishing rather than a natural timeout.
    $master config set replica-output-buffer-throttling "1024 1kb 30mb 60000"

    test {Replica finishing sync during a pause unblocks postponed clients} {
        set rd [redis_deferring_client]
        $rd set before-throttle 1
        assert_equal OK [$rd read]

        # A short, fixed delay instead of arm_replica_request_throttling's
        # helper: rdb-key-save-delay is only read once, when the BGSAVE
        # child forks, so a later CONFIG SET on the parent can't shrink an
        # already-running save. Pick a delay just long enough to arm
        # throttling before the save finishes on its own.
        $master config set rdb-key-save-delay 10000
        populate 150 "" 100

        set loglines [count_log_lines 0]
        $replica replicaof $master_host $master_port
        wait_for_condition 50 100 {
            ([s 0 rdb_bgsave_in_progress] == 1) &&
            [lindex [$replica role] 3] eq {sync}
        } else {
            fail "replica did not enter full sync"
        }

        set load [start_write_load $master_host $master_port 10 "" 1024]
        wait_for_log_messages 0 {"*triggered request throttling*"} $loglines 50 100
        stop_write_load $load

        set start [clock milliseconds]
        $rd set should-unblock-on-sync-finish 1

        # No CONFIG SET here: the BGSAVE child (~1.5s total) finishes and
        # the replica reaches ONLINE on its own.
        wait_replica_online $master

        assert_equal OK [$rd read]
        assert_lessthan [expr {[clock milliseconds] - $start}] 5000
        $rd close
    }

    catch {$replica replicaof no one}
}
}
