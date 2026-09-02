# A manual failover must not leave a sibling replica attached to the old
# master. If the observer replica learns about the failover from the demoted
# master before it sees the new master's slot claim, the slot difference that
# clusterUpdateSlotsConfigWith() relies on has already been consumed, and
# without the safeguard in the demotion path the observer stays a replica of a
# replica.

start_cluster 1 2 {tags {external:skip cluster}} {
    test "Replica follows a manual failover it learns about from the demoted master" {
        set CLUSTERMSG_TYPE_PING 0

        # Node 0 is the master, nodes 1 and 2 are its replicas. Node 1 gets
        # promoted, node 2 is the observer we assert on.
        set master 0
        set promoted 1
        set observer 2

        assert_equal "master" [lindex [R $master role] 0]
        foreach id [list $promoted $observer] {
            assert_equal "slave" [lindex [R $id role] 0]
        }

        set master_id [R $master CLUSTER MYID]
        set promoted_id [R $promoted CLUSTER MYID]
        set observer_id [R $observer CLUSTER MYID]
        assert_equal $master_id [dict get [cluster_get_myself $observer] slaveof]
        set loglines [count_log_lines -$observer]

        # Reproduce the ordering seen in the wild, where the observer processes
        # the old master's demotion before the new master's slot claim. The
        # promoted node must not reach the observer at all:
        #  - CLUSTER FORGET drops the observer from its node table, so it stops
        #    pinging it and clusterBroadcastPong() skips it once the election is
        #    won. The forget propagates, so the old master drops the observer
        #    too, but it still answers the observer's own pings with a PONG,
        #    which is how the demotion gets there.
        #  - dropping PINGs stops the promoted node from answering those pings
        #    with a PONG of its own, which would carry the new slot config.
        # The election is unaffected, it uses FAILOVER_AUTH_*.
        R $promoted CLUSTER FORGET $observer_id
        R $promoted DEBUG DROP-CLUSTER-PACKET-FILTER $CLUSTERMSG_TYPE_PING

        R $promoted CLUSTER FAILOVER

        wait_for_condition 50 100 {
            [lindex [R $promoted role] 0] eq "master"
        } else {
            fail "manual failover did not promote the replica"
        }

        # The observer must recover through the sub-replica safeguard rather
        # than through clusterUpdateSlotsConfigWith(), which by this point can
        # no longer see a slot difference to act on.
        wait_for_log_messages -$observer {"*I'm a sub-replica!*"} $loglines 100 100
        R $promoted DEBUG DROP-CLUSTER-PACKET-FILTER -1

        # It must replicate the new master directly, rather than the old master
        # which is now itself a replica, and the link must actually come up.
        wait_for_condition 100 100 {
            [dict get [cluster_get_myself $observer] slaveof] eq $promoted_id
        } else {
            fail "observer is still a replica of the old master (sub-replica)"
        }
        wait_for_condition 50 100 {
            [string match "*master_link_status:up*" [R $observer info replication]]
        } else {
            fail "observer did not replicate from the new master"
        }
    }
}
