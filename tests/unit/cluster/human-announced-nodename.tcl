# Check if cluster's view of human announced nodename is reported in logs
start_cluster 3 0 {tags {external:skip cluster}} {
    test "Set cluster human announced nodename and let it propagate" {
        for {set j 0} {$j < [llength $::servers]} {incr j} {
            R $j config set cluster-announce-hostname "host-$j.com"
            R $j config set cluster-announce-human-nodename "nodename-$j"
        }

        # We wait for everyone to agree on the hostnames. Since they are gossiped
        # the same way as nodenames, it implies everyone knows the nodenames too.
        wait_for_condition 50 100 {
            [are_hostnames_propagated "host-*.com"] eq 1
        } else {
            fail "cluster hostnames were not propagated"
        }
    }

    test "Human nodenames are visible in log messages" {
        # Pause instance 0, so everyone thinks it is dead
        pause_process [srv 0 pid]

        # We're going to use a message we will know will be sent, node unreachable,
        # since it includes the other node gossiping.
        wait_for_log_messages -1 {"*Node * (nodename-2) reported node * (nodename-0) as not reachable*"} 0 20 500
        wait_for_log_messages -2 {"*Node * (nodename-1) reported node * (nodename-0) as not reachable*"} 0 20 500
        
        resume_process [srv 0 pid]
    }
}
