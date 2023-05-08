# Primitive tests on cluster-enabled redis with modules

source tests/support/cli.tcl

# cluster creation is complicated with TLS, and the current tests don't really need that coverage
tags {tls:skip external:skip cluster modules} {

set testmodule_nokey [file normalize tests/modules/blockonbackground.so]
set testmodule_blockedclient [file normalize tests/modules/blockedclient.so]
set testmodule [file normalize tests/modules/blockonkeys.so]

set modules [list loadmodule $testmodule loadmodule $testmodule_nokey loadmodule $testmodule_blockedclient]
start_cluster 3 0 [list config_lines $modules] {

    set node1 [srv 0 client]
    set node2 [srv -1 client]
    set node3 [srv -2 client]
    set node3_pid [srv -2 pid]

    test "Run blocking command (blocked on key) on cluster node3" {
        # key9184688 is mapped to slot 10923 (first slot of node 3)
        set node3_rd [redis_deferring_client -2]
        $node3_rd fsl.bpop key9184688 0
        $node3_rd flush
        wait_for_condition 50 100 {
            [s -2 blocked_clients] eq {1}
        } else {
            fail "Client executing blocking command (blocked on key) not blocked"
        }
    }

    test "Run blocking command (no keys) on cluster node2" {
        set node2_rd [redis_deferring_client -1]
        $node2_rd block.block 0
        $node2_rd flush

        wait_for_condition 50 100 {
            [s -1 blocked_clients] eq {1}
        } else {
            fail "Client executing blocking command (no keys) not blocked"
        }
    }


    test "Perform a Resharding" {
        exec src/redis-cli --cluster-yes --cluster reshard 127.0.0.1:[srv -2 port] \
                           --cluster-to [$node1 cluster myid] \
                           --cluster-from [$node3 cluster myid] \
                           --cluster-slots 1
    }

    test "Verify command (no keys) is unaffected after resharding" {
        # verify there are blocked clients on node2
        assert_equal [s -1 blocked_clients]  {1}

        #release client 
        $node2 block.release 0
    }

    test "Verify command (blocked on key) got unblocked after resharding" {
        # this (read) will wait for the node3 to realize the new topology
        assert_error {*MOVED*} {$node3_rd read}

        # verify there are no blocked clients
        assert_equal [s 0 blocked_clients]  {0}
        assert_equal [s -1 blocked_clients]  {0}
        assert_equal [s -2 blocked_clients]  {0}
    }

    test "Wait for cluster to be stable" {
        wait_for_condition 1000 50 {
            [catch {exec src/redis-cli --cluster check 127.0.0.1:[srv 0 port]}] == 0 &&
            [catch {exec src/redis-cli --cluster check 127.0.0.1:[srv -1 port]}] == 0 &&
            [catch {exec src/redis-cli --cluster check 127.0.0.1:[srv -2 port]}] == 0 &&
            [CI 0 cluster_state] eq {ok} &&
            [CI 1 cluster_state] eq {ok} &&
            [CI 2 cluster_state] eq {ok}
        } else {
            fail "Cluster doesn't stabilize"
        }
    }

    test "Sanity test push cmd after resharding" {
        assert_error {*MOVED*} {$node3 fsl.push key9184688 1}

        set node1_rd [redis_deferring_client 0]
        $node1_rd fsl.bpop key9184688 0
        $node1_rd flush

        wait_for_condition 50 100 {
            [s 0 blocked_clients] eq {1}
        } else {
            puts "Client not blocked"
            puts "read from blocked client: [$node1_rd read]"
            fail "Client not blocked"
        }

        $node1 fsl.push key9184688 2
        assert_equal {2} [$node1_rd read]
    }

    $node1_rd close
    $node2_rd close
    $node3_rd close

    test "Run blocking command (blocked on key) again on cluster node1" {
        $node1 del key9184688
        # key9184688 is mapped to slot 10923 which has been moved to node1
        set node1_rd [redis_deferring_client 0]
        $node1_rd fsl.bpop key9184688 0
        $node1_rd flush

        wait_for_condition 50 100 {
            [s 0 blocked_clients] eq {1}
        } else {
            fail "Client executing blocking command (blocked on key) again not blocked"
        }
    }

    test "Run blocking command (no keys) again on cluster node2" {
        set node2_rd [redis_deferring_client -1]

        $node2_rd block.block 0
        $node2_rd flush

        wait_for_condition 50 100 {
            [s -1 blocked_clients] eq {1}
        } else {
            fail "Client executing blocking command (no keys) again not blocked"
        }
    }

    test "Kill a cluster node and wait for fail state" {
        # kill node3 in cluster
        pause_process $node3_pid

        wait_for_condition 1000 50 {
            [CI 0 cluster_state] eq {fail} &&
            [CI 1 cluster_state] eq {fail}
        } else {
            fail "Cluster doesn't fail"
        }
    }

    test "Verify command (blocked on key) got unblocked after cluster failure" {
        assert_error {*CLUSTERDOWN*} {$node1_rd read}
    }

    test "Verify command (no keys) got unblocked after cluster failure" {
        assert_error {*CLUSTERDOWN*} {$node2_rd read}

        # verify there are no blocked clients
        assert_equal [s 0 blocked_clients]  {0}
        assert_equal [s -1 blocked_clients]  {0}
    }

    test "Verify command RM_Call is rejected when cluster is down" {
        assert_error "ERR Can not execute a command 'set' while the cluster is down" {$node1 do_rm_call set x 1}
    }

    resume_process $node3_pid
    $node1_rd close
    $node2_rd close
}

set modules [list loadmodule [file normalize tests/modules/keyspace_events.so]]
start_cluster 2 2 [list config_lines $modules] {

    set master1 [srv 0 client]
    set master2 [srv -1 client]
    set replica1 [srv -2 client]
    set replica2 [srv -3 client]
    
    test "Verify keys deletion and notification effects happened on cluster slots change are replicated inside multi exec" {
        $master2 set count_dels_{4oi} 1
        $master2 del count_dels_{4oi}
        assert_equal 1 [$master2 keyspace.get_dels]
        assert_equal 1 [$replica2 keyspace.get_dels]
        $master2 set count_dels_{4oi} 1
        
        set repl [attach_to_replication_stream_on_connection -3]

        $master1 cluster bumpepoch
        $master1 cluster setslot 16382 node [$master1 cluster myid]

        wait_for_cluster_propagation
        wait_for_condition 50 100 {
            [$master2 keyspace.get_dels] eq 2
        } else {
            fail "master did not delete the key"
        }
        wait_for_condition 50 100 {
            [$replica2 keyspace.get_dels] eq 2
        } else {
            fail "replica did not increase del counter"
        }

        assert_replication_stream $repl {
            {multi}
            {del count_dels_{4oi}}
            {keyspace.incr_dels}
            {exec}
        }
        close_replication_stream $repl
    }
}

}

set testmodule [file normalize tests/modules/basics.so]
set modules [list loadmodule $testmodule]
start_cluster 3 0 [list config_lines $modules] {
    set node1 [srv 0 client]
    set node2 [srv -1 client]
    set node3 [srv -2 client]

    test "Verify RM_Call inside module load function on cluster mode" {
        assert_equal {PONG} [$node1 PING]
        assert_equal {PONG} [$node2 PING]
        assert_equal {PONG} [$node3 PING]
    }
}
