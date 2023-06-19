# Check if cluster's view of human announced nodename is reported in logs
start_cluster 3 0 {tags {external:skip cluster}} {
    test "Set cluster human announced nodename and let it propagate" {
        for {set j 0} {$j < [llength $::servers]} {incr j} {
            R $j config set cluster-announce-human-nodename "nodename-$j"
        }

        # We wait for everyone to agree on the epoch bump, which means everyone
        # has exchanged messages so they know about the nodenames.
        R 0 CLUSTER BUMPEPOCH
        wait_for_condition 1000 50 {
            [CI 0 cluster_current_epoch] == [CI 1 cluster_current_epoch] &&
            [CI 0 cluster_current_epoch] == [CI 2 cluster_current_epoch]
        } else {
            fail "Cluster did not converge"
        }
    }

    test "Human nodenames are visible in log messages" {
        # Pause instance 0, so everyone thinks it is dead
        pause_process [srv 0 pid]

        # We're going to use a message we will know will be sent, node unreachable,
        # since it includes the other node gossiping.
        wait_for_log_messages -1 {"*Node * (nodename-2) reported node * (nodename-0) as not reachable*"} 0 10 500
        wait_for_log_messages -2 {"*Node * (nodename-1) reported node * (nodename-0) as not reachable*"} 0 10 500
        
        resume_process [srv 0 pid]
    }
}
