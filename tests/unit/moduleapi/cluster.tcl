# Primitive tests on cluster-enabled redis with modules using redis-cli

source tests/support/cli.tcl

set testmodule [file normalize tests/modules/blockonkeys.so]

# make sure the test infra won't use SELECT
set ::singledb 1

# start three servers
start_server {overrides {cluster-enabled yes} tags {"external:skip cluster modules"}} {
start_server {overrides {cluster-enabled yes} tags {"external:skip cluster modules"}} {
start_server {overrides {cluster-enabled yes} tags {"external:skip cluster modules"}} {

    set node1 [srv 0 client]
    set node2 [srv -1 client]
    set node3 [srv -2 client]

    $node1 module load $testmodule
    $node2 module load $testmodule
    $node3 module load $testmodule

    test {Create 3 node cluster} {
        exec src/redis-cli --cluster-yes --cluster create \
                           127.0.0.1:[srv 0 port] \
                           127.0.0.1:[srv -1 port] \
                           127.0.0.1:[srv -2 port]
    }

    test "Run blocking command on cluster node3" {
        # key9184688 is mapped to slot 10923 (first slot of node 3)
        # use DEL command to wait for the cluster to be stable
        wait_for_condition 1000 50 {
            ![catch {$node3 del key9184688}]
        } else {
            fail "Cluster doesn't stabilize"
        }

        set node3_rd [redis_deferring_client -2]
        $node3_rd fsl.bpop key9184688 0
        $node3_rd flush

        wait_for_condition 50 100 {
            [s -2 blocked_clients] eq {1}
        } else {
            fail "Client not blocked"
        }
    }

    test "Perform a Resharding" {
        exec src/redis-cli --cluster-yes --cluster reshard 127.0.0.1:[srv -2 port] \
                           --cluster-to [$node1 cluster myid] \
                           --cluster-from [$node3 cluster myid] \
                           --cluster-slots 1
    }

    test "Verify command got unblocked after resharding" {
        # this (read) will wait for the node3 to realize the new topology
        assert_error {*MOVED*} {$node3_rd read}

        # verify there are no blocked clients
        assert_equal [s 0 blocked_clients]  {0}
        assert_equal [s -1 blocked_clients]  {0}
        assert_equal [s -2 blocked_clients]  {0}
    }

    test "wait for cluster to be stable" {
        # use EXISTS command to wait for the cluster to be stable
        wait_for_condition 1000 50 {
            ![catch {$node1 exists key9184688}]
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
    $node3_rd close

# stop three servers
}
}
}
