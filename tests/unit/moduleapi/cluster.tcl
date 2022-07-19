# Primitive tests on cluster-enabled redis with modules using redis-cli

source tests/support/cli.tcl

set testmodule [file normalize tests/modules/blockonkeys.so]
set testmodule_nokey [file normalize tests/modules/blockonbackground.so]
set testmodule_blockedclient [file normalize tests/modules/blockedclient.so]

# make sure the test infra won't use SELECT
set old_singledb $::singledb
set ::singledb 1

# cluster creation is complicated with TLS, and the current tests don't really need that coverage
tags {tls:skip external:skip cluster modules} {

# start three servers
set base_conf [list cluster-enabled yes cluster-node-timeout 1 loadmodule $testmodule]
start_server [list overrides $base_conf] {
start_server [list overrides $base_conf] {
start_server [list overrides $base_conf] {

    set node1 [srv 0 client]
    set node2 [srv -1 client]
    set node3 [srv -2 client]
    set node3_pid [srv -2 pid]

    # the "overrides" mechanism can only support one "loadmodule" directive
    $node1 module load $testmodule_nokey
    $node2 module load $testmodule_nokey
    $node3 module load $testmodule_nokey

    $node1 module load $testmodule_blockedclient
    $node2 module load $testmodule_blockedclient
    $node3 module load $testmodule_blockedclient

    test {Create 3 node cluster} {
        exec src/redis-cli --cluster-yes --cluster create \
                           127.0.0.1:[srv 0 port] \
                           127.0.0.1:[srv -1 port] \
                           127.0.0.1:[srv -2 port]

        wait_for_condition 1000 50 {
            [csi 0 cluster_state] eq {ok} &&
            [csi -1 cluster_state] eq {ok} &&
            [csi -2 cluster_state] eq {ok}
        } else {
            fail "Cluster doesn't stabilize"
        }
    }

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
        exec kill -SIGSTOP $node3_pid

        wait_for_condition 1000 50 {
            [csi 0 cluster_state] eq {fail} &&
            [csi -1 cluster_state] eq {fail}
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

    exec kill -SIGCONT $node3_pid
    $node1_rd close
    $node2_rd close

# stop three servers
}
}
}

} ;# tags

set ::singledb $old_singledb
