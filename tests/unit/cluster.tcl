# Primitive tests on cluster-enabled redis using redis-cli

source tests/support/cli.tcl

proc cluster_info {r field} {
    if {[regexp "^$field:(.*?)\r\n" [$r cluster info] _ value]} {
        set _ $value
    }
}

# Provide easy access to CLUSTER INFO properties. Same semantic as "proc s".
proc csi {args} {
    set level 0
    if {[string is integer [lindex $args 0]]} {
        set level [lindex $args 0]
        set args [lrange $args 1 end]
    }
    cluster_info [srv $level "client"] [lindex $args 0]
}

# make sure the test infra won't use SELECT
set ::singledb 1

# cluster creation is complicated with TLS, and the current tests don't really need that coverage
tags {tls:skip external:skip cluster} {

# start three servers
set base_conf [list cluster-enabled yes cluster-node-timeout 1]
start_server [list overrides $base_conf] {
start_server [list overrides $base_conf] {
start_server [list overrides $base_conf] {

    set node1 [srv 0 client]
    set node2 [srv -1 client]
    set node3 [srv -2 client]
    set node3_pid [srv -2 pid]

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

    test "Run blocking command on cluster node3" {
        # key9184688 is mapped to slot 10923 (first slot of node 3)
        set node3_rd [redis_deferring_client -2]
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
       wait_for_condition 1000 50 {
            [catch {exec src/redis-cli --cluster \
            check 127.0.0.1:[srv 0 port] \
            }] == 0
        } else {
            fail "Cluster doesn't stabilize"
        }
    }

    test "Sanity test push cmd after resharding" {
        assert_error {*MOVED*} {$node3 lpush key9184688 v1}

        set node1_rd [redis_deferring_client 0]
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

    $node1_rd close
    $node3_rd close
    
    test "Run blocking command again on cluster node1" {
        $node1 del key9184688
        # key9184688 is mapped to slot 10923 which has been moved to node1
        set node1_rd [redis_deferring_client 0]
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
        exec kill -SIGSTOP $node3_pid

        wait_for_condition 1000 50 {
            [csi 0 cluster_state] eq {fail} &&
            [csi -1 cluster_state] eq {fail}
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

    exec kill -SIGCONT $node3_pid
    $node1_rd close

# stop three servers
}
}
}

} ;# tags