start_server {tags {"failover"}} {
start_server {} {
start_server {} {
    set node_0 [srv 0 client]
    set node_0_host [srv 0 host]
    set node_0_port [srv 0 port]

    set node_1 [srv -1 client]
    set node_1_host [srv -1 host]
    set node_1_port [srv -1 port]

    set node_2 [srv -2 client]
    set node_2_host [srv -2 host]
    set node_2_port [srv -2 port]

    test {failover command fails without connected replica} {
        catch { $node_0 failover to $node_1_host $node_1_port } err
        if {! [string match "ERR*" $err]} {
            fail "failover command succeeded when replica not connected"
        }
    }

    test {setup replication for following tests} {
        $node_1 replicaof $node_0_host $node_0_port
        $node_2 replicaof $node_0_host $node_0_port
        wait_for_sync $node_1
        wait_for_sync $node_2
    }

    test {failover command fails with invalid host} {
        catch { $node_0 failover to invalidhost $node_1_port } err
        if {! [string match "ERR*" $err]} {
            fail "failover command succeeded with invalid host"
        }
    }

    test {failover command fails with invalid port} {
        catch { $node_0 failover to $node_1_host invalidport } err
        if {! [string match "ERR*" $err]} {
            fail "failover command succeeded with invalid port"
        }
    }

    test {failover command fails with any one and force} {
        catch { $node_0 failover to ANY ONE FORCE TIMEOUT 100} err
        if {! [string match "ERR*" $err]} {
            fail "failover command succeeded with invalid port"
        }
    }

    test {failover command fails when sent to a replica} {
        catch { $node_1 failover to $node_1_host $node_1_port } err
        if {! [string match "ERR*" $err]} {
            fail "failover command succeeded when sent to replica"
        }
    }

    test {failover command fails with force without timeout} {
        catch { $node_0 failover to $node_1_host $node_1_port FORCE } err
        if {! [string match "ERR*" $err]} {
            fail "failover command succeeded when sent to replica"
        }
    }

    test {failover command to specific replica works} {
        set initial_psyncs [s -1 sync_partial_ok]
        set initial_syncs [s -1 sync_full]

        # Generate a delta between primary and replica
        exec kill -SIGSTOP [srv -1 pid]
        set bench_pid [exec src/redis-benchmark -s [srv 0 unixsocket] -c 50 -n 1000 -r 1 INCR FOO > /tmp/whatever &]
        wait_for_condition 50 100 {
            [s 0 total_commands_processed] > 100
        } else {
            fail "Node 0 did not accept writes"
        }
        exec kill -SIGCONT [srv -1 pid]

        # Execute the failover
        $node_0 failover to $node_1_host $node_1_port

        # Wait for failover to end
        wait_for_condition 50 100 {
            [s 0 master_failover_state] == "no-failover"
        } else {
            fail "Failover from node 0 to node 1 did not finish"
        }

        $node_2 replicaof $node_1_host $node_1_port
        wait_for_sync $node_0
        wait_for_sync $node_2

        assert_match *slave* [$node_0 role]
        assert_match *master* [$node_1 role]
        assert_match *slave* [$node_2 role]

        # We should accept psyncs from both nodes
        assert_equal [expr [s -1 sync_partial_ok] - $initial_psyncs] 2
        assert_equal [expr [s -1 sync_full] - $initial_psyncs] 0
    }

    test {failover command to any replica works} {
        set initial_psyncs [s -2 sync_partial_ok]
        set initial_syncs [s -2 sync_full]

        wait_for_ofs_sync $node_1 $node_2
        # We stop node 0 to and make sure node 2 is selected
        exec kill -SIGSTOP [srv 0 pid]
        $node_1 set CASE 1
        $node_1 failover to any one

        # Wait for failover to end
        wait_for_condition 50 100 {
            [s -1 master_failover_state] == "no-failover"
        } else {
            fail "Failover from node 1 to node 2 did not finish"
        }
        exec kill -SIGCONT [srv 0 pid]
        $node_0 replicaof $node_2_host $node_2_port

        wait_for_sync $node_0
        wait_for_sync $node_1

        assert_match *slave* [$node_0 role]
        assert_match *slave* [$node_1 role]
        assert_match *master* [$node_2 role]

        # We should accept Psyncs from both nodes
        assert_equal [expr [s -2 sync_partial_ok] - $initial_psyncs] 2
        assert_equal [expr [s -1 sync_full] - $initial_psyncs] 0
    }

    test {failover to a replica with force works} {
        set initial_psyncs [s 0 sync_partial_ok]
        set initial_syncs [s 0 sync_full]

        exec kill -SIGSTOP [srv 0 pid]
        # node 0 will never acknowledge this write
        $node_2 set case 2
        $node_2 failover to $node_0_host $node_0_port TIMEOUT 10 FORCE
        exec kill -SIGCONT [srv 0 pid]

        # Wait for failover to end
        wait_for_condition 50 100 {
            [s -2 master_failover_state] == "no-failover"
        } else {
            fail "Failover from node 2 to node 0 did not finish"
        }

        $node_1 replicaof $node_0_host $node_0_port

        wait_for_sync $node_1
        wait_for_sync $node_2

        assert_match *master* [$node_0 role]
        assert_match *slave* [$node_1 role]
        assert_match *slave* [$node_2 role]

        # We should accept Psyncs from both nodes
        assert_equal [expr [s 0 sync_partial_ok] - $initial_psyncs] 2
        assert_equal [expr [s 0 sync_full] - $initial_syncs] 0
    }

    test {failover with timeout aborts if replica never catches up} {
        set initial_psyncs [s 0 sync_partial_ok]
        set initial_syncs [s 0 sync_full]

        # Stop replica so it never catches up
        exec kill -SIGSTOP [srv -1 pid]
        $node_0 SET CASE 1
        
        $node_0 failover to [srv -1 host] [srv -1 port] TIMEOUT 500
        # Wait for failover to end
        wait_for_condition 50 20 {
            [s 0 master_failover_state] == "no-failover"
        } else {
            fail "Failover from node_0 to replica did not finish"
        }

        exec kill -SIGCONT [srv -1 pid]

        # We need to make sure the nodes actually sync back up
        wait_for_ofs_sync $node_0 $node_1
        wait_for_ofs_sync $node_0 $node_2

        assert_match *master* [$node_0 role]
        assert_match *slave* [$node_1 role]
        assert_match *slave* [$node_2 role]

        # Since we never caught up, there should be no syncs
        assert_equal [expr [s 0 sync_partial_ok] - $initial_psyncs] 0
        assert_equal [expr [s 0 sync_full] - $initial_syncs] 0
    }

    test {failovers can be aborted} {
        set initial_psyncs [s 0 sync_partial_ok]
        set initial_syncs [s 0 sync_full]
    
        # Stop replica so it never catches up
        exec kill -SIGSTOP [srv -1 pid]
        $node_0 SET CASE 2
        
        $node_0 failover to [srv -1 host] [srv -1 port] TIMEOUT 60000
        assert_match [s 0 master_failover_state] "waiting-for-sync"

        # Sanity check that read commands are still accepted
        $node_0 GET CASE

        $node_0 failover abort
        assert_match [s 0 master_failover_state] "no-failover"

        exec kill -SIGCONT [srv -1 pid]

        # Just make sure everything is still synced
        wait_for_ofs_sync $node_0 $node_1
        wait_for_ofs_sync $node_0 $node_2

        assert_match *master* [$node_0 role]
        assert_match *slave* [$node_1 role]
        assert_match *slave* [$node_2 role]

        # Since we never caught up, there should be no syncs
        assert_equal [expr [s 0 sync_partial_ok] - $initial_psyncs] 0
        assert_equal [expr [s 0 sync_full] - $initial_syncs] 0
    }

    test {failover aborts if target rejects sync request} {
        set initial_psyncs [s 0 sync_partial_ok]
        set initial_syncs [s 0 sync_full]

        # We block psync, so the failover will fail
        $node_1 acl setuser default -psync

        # We pause the target long enough to send a write command
        # during the pause. This write will not be interrupted.
        exec kill -SIGSTOP [srv -1 pid]
        set rd [redis_deferring_client]
        $rd SET FOO BAR
        $node_0 failover to $node_1_host $node_1_port
        exec kill -SIGCONT [srv -1 pid]

        # Wait for failover to end
        wait_for_condition 50 100 {
            [s 0 master_failover_state] == "no-failover"
        } else {
            fail "Failover from node_0 to replica did not finish"
        }

        assert_equal [$rd read] "OK"
        $rd close

        # restore access to psync
        $node_1 acl setuser default +psync

        # We need to make sure the nodes actually sync back up
        wait_for_sync $node_1
        wait_for_sync $node_2

        assert_match *master* [$node_0 role]
        assert_match *slave* [$node_1 role]
        assert_match *slave* [$node_2 role]

        # We will cycle all of our replicas here and force a psync.
        assert_equal [expr [s 0 sync_partial_ok] - $initial_psyncs] 2
        assert_equal [expr [s 0 sync_full] - $initial_syncs] 0

        assert_equal [count_log_message 0 "Failover target rejected psync request"] 1
    }
}
}
}
