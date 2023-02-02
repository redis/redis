
proc number_of_peers {id} {
    expr [llength $::servers] - 1
}

proc number_of_links {id} {
    llength [R $id cluster links]
}

proc publish_messages {server num_msgs msg_size} {
    for {set i 0} {$i < $num_msgs} {incr i} {
        $server PUBLISH channel [string repeat "x" $msg_size]
    }
}

start_cluster 1 2 {tags {external:skip cluster}} {
    set primary_id 0
    set replica1_id 1

    set primary [Rn $primary_id]
    set replica1 [Rn $replica1_id]

    test "Broadcast message across a cluster shard while a cluster link is down" {
        set replica1_node_id [$replica1 CLUSTER MYID]

        set channelname ch3

        # subscribe on replica1
        set subscribeclient1 [redis_deferring_client -1]
        $subscribeclient1 deferred 1
        $subscribeclient1 SSUBSCRIBE $channelname
        $subscribeclient1 read

        # subscribe on replica2
        set subscribeclient2 [redis_deferring_client -2]
        $subscribeclient2 deferred 1
        $subscribeclient2 SSUBSCRIBE $channelname
        $subscribeclient2 read

        # Verify number of links with cluster stable state
        assert_equal [expr [number_of_peers $primary_id]*2] [number_of_links $primary_id]

        # Disconnect the cluster between primary and replica1 and publish a message.
        $primary MULTI
        $primary DEBUG CLUSTERLINK KILL TO $replica1_node_id
        $primary SPUBLISH $channelname hello
        set res [$primary EXEC]

        # Verify no client exists on the primary to receive the published message.
        assert_equal $res {OK 0}

        # Wait for all the cluster links are healthy
        wait_for_condition 50 100 {
            [number_of_peers $primary_id]*2 == [number_of_links $primary_id]
        } else {
            fail "All peer links couldn't be established"
        }

        # Publish a message afterwards.
        $primary SPUBLISH $channelname world

        # Verify replica1 has received only (world) / hello is lost.
        assert_equal "smessage ch3 world" [$subscribeclient1 read]

        # Verify replica2 has received both messages (hello/world)
        assert_equal "smessage ch3 hello" [$subscribeclient2 read]
        assert_equal "smessage ch3 world" [$subscribeclient2 read]
    } {} {needs:debug}
}

