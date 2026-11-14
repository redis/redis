# Votes (FAILOVER_AUTH_ACK) that arrive after an election timed out must not be
# counted toward the replica's next election, otherwise it can win an epoch
# without a majority of the masters voting for it in that epoch.
start_cluster 3 1 {tags {external:skip cluster} overrides {cluster-node-timeout 1000}} {
    test "Votes for an expired election are not counted toward the next one" {
        set master0_id [R 0 CLUSTER MYID]
        set replica_id [R 3 CLUSTER MYID]
        wait_for_sync [srv -3 client]

        set replica_loglines [count_log_lines -3]

        # Fail the replica's master and wait for both voters to see it as
        # failed and for the replica to schedule its election.
        pause_process [srv 0 pid]
        wait_node_marked_fail 1 $master0_id
        wait_node_marked_fail 2 $master0_id
        wait_for_log_messages -3 {"*Start of election delayed*"} $replica_loglines 1000 10

        # Stall both voters so the vote request of this election is only
        # processed after it has timed out and the replica retried.
        pause_process [srv -1 pid]
        pause_process [srv -2 pid]
        set replica_loglines [count_log_lines -3]
        wait_for_log_messages -3 {"*Starting a failover election*"} $replica_loglines 1000 10
        set replica_loglines [count_log_lines -3]
        wait_for_log_messages -3 {"*Start of election delayed*"} $replica_loglines 1000 10
        resume_process [srv -1 pid]
        resume_process [srv -2 pid]

        wait_for_condition 1000 50 {
            [s -3 role] eq {master}
        } else {
            fail "The replica wasn't promoted"
        }

        # The replica must have won with a majority of votes in its epoch.
        set epoch [dict get [cluster_get_myself 3] config_epoch]
        set votes 0
        foreach idx {1 2} {
            set fp [open [srv -$idx stdout] r]
            set content [read $fp]
            close $fp
            if {[regexp "Failover auth granted to $replica_id \[^\n\]* for epoch $epoch\n" $content]} {
                incr votes
            }
        }
        assert_equal 2 $votes

        resume_process [srv 0 pid]
    }
}
