start_server {tags {"pause external:skip"}} {
start_server {} {
    set node_0 [srv 0 client]
    set node_0_host [srv 0 host]
    set node_0_port [srv 0 port]
    set node_0_pid [srv 0 pid]

    set node_1 [srv -1 client]
    set node_1_host [srv -1 host]
    set node_1_port [srv -1 port]
    set node_1_pid [srv -1 pid]

    proc assert_digests_match {n1 n2} {
        assert_equal [$n1 debug digest] [$n2 debug digest]
    }

    test {pause client after failover until timeout} {
        $node_1 replicaof $node_0_host $node_0_port
        wait_for_sync $node_1

        # Wait for failover to end
        $node_0 failover to $node_1_host $node_1_port
        wait_for_condition 50 100 {
            [s 0 master_failover_state] == "no-failover"
        } else {
            fail "Failover from node_0 to node_1 did not finish"
        }
        $node_1 failover to $node_0_host $node_0_port
        wait_for_condition 50 100 {
            [s -1 master_failover_state] == "no-failover"
        } else {
            fail "Failover from node_1 to node_0 did not finish"
        }

        # Write commands are paused after failover until timeout
        $node_0 client pause 500 write
        set rd [redis_deferring_client 0]
        $rd set a 1
        wait_for_blocked_clients_count 0 10 100
        assert_match [$rd read] "OK"
        $rd close
    }
}
}
