# Primitive tests on cluster-enabled redis using redis-cli

source tests/support/cli.tcl

# make sure the test infra won't use SELECT
set old_singledb $::singledb
set ::singledb 1

# cluster creation is complicated with TLS, and the current tests don't really need that coverage
tags {tls:skip external:skip cluster} {

# start three servers
set base_conf [list cluster-enabled yes cluster-node-timeout 1000]
start_multiple_servers 3 [list overrides $base_conf] {

    set node1 [srv 0 client]
    set node2 [srv -1 client]
    set node3 [srv -2 client]
    set node3_pid [srv -2 pid]
    set node3_rd [redis_deferring_client -2]

    test {Create 3 node cluster} {
        exec src/redis-cli --cluster-yes --cluster create \
                           127.0.0.1:[srv 0 port] \
                           127.0.0.1:[srv -1 port] \
                           127.0.0.1:[srv -2 port]

        wait_for_condition 1000 50 {
            [CI 0 cluster_state] eq {ok} &&
            [CI 1 cluster_state] eq {ok} &&
            [CI 2 cluster_state] eq {ok}
        } else {
            fail "Cluster doesn't stabilize"
        }
    }

    test "Run blocking command on cluster node3" {
        # key9184688 is mapped to slot 10923 (first slot of node 3)
        $node3_rd brpop key9184688 0
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

    test "Wait for cluster to be stable" {
        # Cluster check just verifies the config state is self-consistent,
        # waiting for cluster_state to be okay is an independent check that all the
        # nodes actually believe each other are healthy, prevent cluster down error.
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

    set node1_rd [redis_deferring_client 0]

    test "use previous hostip in \"cluster-preferred-endpoint-type unknown-endpoint\" mode" {
        
        # backup and set cluster-preferred-endpoint-type unknown-endpoint
        set endpoint_type_before_set [lindex [split [$node1 CONFIG GET cluster-preferred-endpoint-type] " "] 1]
        $node1 CONFIG SET cluster-preferred-endpoint-type unknown-endpoint

        # when redis-cli not in cluster mode, return MOVE with empty host
        set slot_for_foo [$node1 CLUSTER KEYSLOT foo]
        assert_error "*MOVED $slot_for_foo :*" {$node1 set foo bar}

        # when in cluster mode, redirect using previous hostip
        assert_equal "[exec src/redis-cli -h 127.0.0.1 -p [srv 0 port] -c set foo bar]" {OK}
        assert_match "[exec src/redis-cli -h 127.0.0.1 -p [srv 0 port] -c get foo]" {bar}

        assert_equal [$node1 CONFIG SET cluster-preferred-endpoint-type "$endpoint_type_before_set"]  {OK}
    }

    test "Sanity test push cmd after resharding" {
        assert_error {*MOVED*} {$node3 lpush key9184688 v1}

        $node1_rd brpop key9184688 0
        $node1_rd flush

        wait_for_condition 50 100 {
            [s 0 blocked_clients] eq {1}
        } else {
            puts "Client not blocked"
            puts "read from blocked client: [$node1_rd read]"
            fail "Client not blocked"
        }

        $node1 lpush key9184688 v2
        assert_equal {key9184688 v2} [$node1_rd read]
    }

    $node3_rd close

    test "Run blocking command again on cluster node1" {
        $node1 del key9184688
        # key9184688 is mapped to slot 10923 which has been moved to node1
        $node1_rd brpop key9184688 0
        $node1_rd flush

        wait_for_condition 50 100 {
            [s 0 blocked_clients] eq {1}
        } else {
            fail "Client not blocked"
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

     test "Verify command got unblocked after cluster failure" {
        assert_error {*CLUSTERDOWN*} {$node1_rd read}

        # verify there are no blocked clients
        assert_equal [s 0 blocked_clients]  {0}
        assert_equal [s -1 blocked_clients]  {0}
    }

    resume_process $node3_pid
    $node1_rd close

} ;# stop servers

# Test redis-cli -- cluster create, add-node, call.
# Test that functions are propagated on add-node
start_multiple_servers 5 [list overrides $base_conf] {

    set node4_rd [redis_client -3]
    set node5_rd [redis_client -4]

    test {Functions are added to new node on redis-cli cluster add-node} {
        exec src/redis-cli --cluster-yes --cluster create \
                           127.0.0.1:[srv 0 port] \
                           127.0.0.1:[srv -1 port] \
                           127.0.0.1:[srv -2 port]


        wait_for_condition 1000 50 {
            [CI 0 cluster_state] eq {ok} &&
            [CI 1 cluster_state] eq {ok} &&
            [CI 2 cluster_state] eq {ok}
        } else {
            fail "Cluster doesn't stabilize"
        }

        # upload a function to all the cluster
        exec src/redis-cli --cluster-yes --cluster call 127.0.0.1:[srv 0 port] \
                           FUNCTION LOAD {#!lua name=TEST
                               redis.register_function('test', function() return 'hello' end)
                           }

        # adding node to the cluster
        exec src/redis-cli --cluster-yes --cluster add-node \
                       127.0.0.1:[srv -3 port] \
                       127.0.0.1:[srv 0 port]

        wait_for_cluster_size 4

        wait_for_condition 1000 50 {
            [CI 0 cluster_state] eq {ok} &&
            [CI 1 cluster_state] eq {ok} &&
            [CI 2 cluster_state] eq {ok} &&
            [CI 3 cluster_state] eq {ok}
        } else {
            fail "Cluster doesn't stabilize"
        }

        # make sure 'test' function was added to the new node
        assert_equal {{library_name TEST engine LUA functions {{name test description {} flags {}}}}} [$node4_rd FUNCTION LIST]

        # add function to node 5
        assert_equal {TEST} [$node5_rd FUNCTION LOAD {#!lua name=TEST
            redis.register_function('test', function() return 'hello' end)
        }]

        # make sure functions was added to node 5
        assert_equal {{library_name TEST engine LUA functions {{name test description {} flags {}}}}} [$node5_rd FUNCTION LIST]

        # adding node 5 to the cluster should failed because it already contains the 'test' function
        catch {
            exec src/redis-cli --cluster-yes --cluster add-node \
                        127.0.0.1:[srv -4 port] \
                        127.0.0.1:[srv 0 port]
        } e
        assert_match {*node already contains functions*} $e
    }
} ;# stop servers

# Test redis-cli --cluster create, add-node.
# Test that one slot can be migrated to and then away from the new node.
test {Migrate the last slot away from a node using redis-cli} {
    start_multiple_servers 4 [list overrides $base_conf] {

        # Create a cluster of 3 nodes
        exec src/redis-cli --cluster-yes --cluster create \
                           127.0.0.1:[srv 0 port] \
                           127.0.0.1:[srv -1 port] \
                           127.0.0.1:[srv -2 port]

        wait_for_condition 1000 50 {
            [CI 0 cluster_state] eq {ok} &&
            [CI 1 cluster_state] eq {ok} &&
            [CI 2 cluster_state] eq {ok}
        } else {
            fail "Cluster doesn't stabilize"
        }

        # Insert some data
        assert_equal OK [exec src/redis-cli -c -p [srv 0 port] SET foo bar]
        set slot [exec src/redis-cli -c -p [srv 0 port] CLUSTER KEYSLOT foo]

        # Add new node to the cluster
        exec src/redis-cli --cluster-yes --cluster add-node \
                     127.0.0.1:[srv -3 port] \
                     127.0.0.1:[srv 0 port]
        
        # First we wait for new node to be recognized by entire cluster
        wait_for_cluster_size 4
        
        wait_for_condition 1000 50 {
            [CI 0 cluster_state] eq {ok} &&
            [CI 1 cluster_state] eq {ok} &&
            [CI 2 cluster_state] eq {ok} &&
            [CI 3 cluster_state] eq {ok}
        } else {
            fail "Cluster doesn't stabilize"
        }

        set newnode_r [redis_client -3]
        set newnode_id [$newnode_r CLUSTER MYID]

        # Find out which node has the key "foo" by asking the new node for a
        # redirect.
        catch { $newnode_r get foo } e
        assert_match "MOVED $slot *" $e
        lassign [split [lindex $e 2] :] owner_host owner_port
        set owner_r [redis $owner_host $owner_port 0 $::tls]
        set owner_id [$owner_r CLUSTER MYID]

        # Move slot to new node using plain Redis commands
        assert_equal OK [$newnode_r CLUSTER SETSLOT $slot IMPORTING $owner_id]
        assert_equal OK [$owner_r CLUSTER SETSLOT $slot MIGRATING $newnode_id]
        assert_equal {foo} [$owner_r CLUSTER GETKEYSINSLOT $slot 10]
        assert_equal OK [$owner_r MIGRATE 127.0.0.1 [srv -3 port] "" 0 5000 KEYS foo]
        assert_equal OK [$newnode_r CLUSTER SETSLOT $slot NODE $newnode_id]
        assert_equal OK [$owner_r CLUSTER SETSLOT $slot NODE $newnode_id]

        # Using --cluster check make sure we won't get `Not all slots are covered by nodes`.
        # Wait for the cluster to become stable make sure the cluster is up during MIGRATE.
        wait_for_condition 1000 50 {
            [catch {exec src/redis-cli --cluster check 127.0.0.1:[srv 0 port]}] == 0 &&
            [catch {exec src/redis-cli --cluster check 127.0.0.1:[srv -1 port]}] == 0 &&
            [catch {exec src/redis-cli --cluster check 127.0.0.1:[srv -2 port]}] == 0 &&
            [catch {exec src/redis-cli --cluster check 127.0.0.1:[srv -3 port]}] == 0 &&
            [CI 0 cluster_state] eq {ok} &&
            [CI 1 cluster_state] eq {ok} &&
            [CI 2 cluster_state] eq {ok} &&
            [CI 3 cluster_state] eq {ok}
        } else {
            fail "Cluster doesn't stabilize"
        }

        # Move the only slot back to original node using redis-cli
        exec src/redis-cli --cluster reshard 127.0.0.1:[srv -3 port] \
            --cluster-from $newnode_id \
            --cluster-to $owner_id \
            --cluster-slots 1 \
            --cluster-yes

        # The empty node will become a replica of the new owner before the
        # `MOVED` check, so let's wait for the cluster to become stable.
        wait_for_condition 1000 50 {
            [CI 0 cluster_state] eq {ok} &&
            [CI 1 cluster_state] eq {ok} &&
            [CI 2 cluster_state] eq {ok} &&
            [CI 3 cluster_state] eq {ok}
        } else {
            fail "Cluster doesn't stabilize"
        }

        # Check that the key foo has been migrated back to the original owner.
        catch { $newnode_r get foo } e
        assert_equal "MOVED $slot $owner_host:$owner_port" $e

        # Check that the empty node has turned itself into a replica of the new
        # owner and that the new owner knows that.
        wait_for_condition 1000 50 {
            [string match "*slave*" [$owner_r CLUSTER REPLICAS $owner_id]]
        } else {
            fail "Empty node didn't turn itself into a replica."
        }
    }
}

# Test redis-cli --cluster create, add-node with cluster-port.
# Create five nodes, three with custom cluster_port and two with default values.
start_server [list overrides [list cluster-enabled yes cluster-node-timeout 1 cluster-port [find_available_port $::baseport $::portcount]]] {
start_server [list overrides [list cluster-enabled yes cluster-node-timeout 1]] {
start_server [list overrides [list cluster-enabled yes cluster-node-timeout 1 cluster-port [find_available_port $::baseport $::portcount]]] {
start_server [list overrides [list cluster-enabled yes cluster-node-timeout 1]] {
start_server [list overrides [list cluster-enabled yes cluster-node-timeout 1 cluster-port [find_available_port $::baseport $::portcount]]] {

    # The first three are used to test --cluster create.
    # The last two are used to test --cluster add-node
    set node1_rd [redis_client 0]
    set node2_rd [redis_client -1]
    set node3_rd [redis_client -2]
    set node4_rd [redis_client -3]
    set node5_rd [redis_client -4]

    test {redis-cli --cluster create with cluster-port} {
        exec src/redis-cli --cluster-yes --cluster create \
                           127.0.0.1:[srv 0 port] \
                           127.0.0.1:[srv -1 port] \
                           127.0.0.1:[srv -2 port]

        wait_for_condition 1000 50 {
            [CI 0 cluster_state] eq {ok} &&
            [CI 1 cluster_state] eq {ok} &&
            [CI 2 cluster_state] eq {ok}
        } else {
            fail "Cluster doesn't stabilize"
        }

        # Make sure each node can meet other nodes
        assert_equal 3 [CI 0 cluster_known_nodes]
        assert_equal 3 [CI 1 cluster_known_nodes]
        assert_equal 3 [CI 2 cluster_known_nodes]
    }

    test {redis-cli --cluster add-node with cluster-port} {
        # Adding node to the cluster (without cluster-port)
        exec src/redis-cli --cluster-yes --cluster add-node \
                           127.0.0.1:[srv -3 port] \
                           127.0.0.1:[srv 0 port]

        wait_for_cluster_size 4

        wait_for_condition 1000 50 {
            [CI 0 cluster_state] eq {ok} &&
            [CI 1 cluster_state] eq {ok} &&
            [CI 2 cluster_state] eq {ok} &&
            [CI 3 cluster_state] eq {ok}
        } else {
            fail "Cluster doesn't stabilize"
        }

        # Adding node to the cluster (with cluster-port)
        exec src/redis-cli --cluster-yes --cluster add-node \
                           127.0.0.1:[srv -4 port] \
                           127.0.0.1:[srv 0 port]

        wait_for_cluster_size 5

        wait_for_condition 1000 50 {
            [CI 0 cluster_state] eq {ok} &&
            [CI 1 cluster_state] eq {ok} &&
            [CI 2 cluster_state] eq {ok} &&
            [CI 3 cluster_state] eq {ok} &&
            [CI 4 cluster_state] eq {ok}
        } else {
            fail "Cluster doesn't stabilize"
        }

        # Make sure each node can meet other nodes
        assert_equal 5 [CI 0 cluster_known_nodes]
        assert_equal 5 [CI 1 cluster_known_nodes]
        assert_equal 5 [CI 2 cluster_known_nodes]
        assert_equal 5 [CI 3 cluster_known_nodes]
        assert_equal 5 [CI 4 cluster_known_nodes]
    }
# stop 5 servers
}
}
}
}
}

} ;# tags

set ::singledb $old_singledb
