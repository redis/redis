# Check if cluster's view of human announced nodename is consistent
#Exclude aux fields from "cluster nodes" We may decide to re-introduce them in some form or another in the #future, but not in v7.2
proc are_nodenames_propagated {match_string} {
    for {set j 0} {$j < [llength $::servers]} {incr j} {
        foreach n [get_cluster_nodes $j] {
            if { ![string match $match_string [dict get $n nodename]] } {
		    return 0
	    }
        }
    }
    return 1
}

# Start a cluster with 3 masters.
start_cluster 3 0 {tags {external:skip cluster}} {
    test "Set cluster human announced nodename and verify they are excluded from cluster node" {
        for {set j 0} {$j < [llength $::servers]} {incr j} {
            R $j config set cluster-announce-human-nodename "nodename-$j.com"
        }
        wait_for_condition 50 100 {
            [are_nodenames_propagated "nodename-*.com"] eq 0
        } else {
            fail "cluster human announced nodename were propagated"
        }

        # Now that everything is propagated, assert everyone agrees
        wait_for_cluster_propagation
    }

    test "Update human announced nodename and make sure they are excluded from cluster node" {
        for {set j 0} {$j < [llength $::servers]} {incr j} {
            R $j config set cluster-announce-human-nodename "nodename-updated-$j.com"
        }
        wait_for_condition 50 100 {
            [are_nodenames_propagated "nodename-updated-*.com"] eq 0
        } else {
            fail "cluster human announced nodename were propagated"
        }

        # Now that everything is propagated, assert everyone agrees
        wait_for_cluster_propagation
    }
}
