# Check if cluster's view of human announced nodename is consistent
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
# These tests rely on specific node ordering, so make sure no node fails over.
start_cluster 3 0 {tags {external:skip cluster} overrides {cluster-replica-no-failover yes}} {
test "Set cluster human announced nodename and verify they are propagated" {
    for {set j 0} {$j < [llength $::servers]} {incr j} {
        R $j config set cluster-announce-human-nodename "nodename-$j.com"
    }
    wait_for_condition 50 100 {
        [are_nodenames_propagated "nodename-*.com"] eq 1
    } else {
        fail "cluster human announced nodename were not propagated"
    }

    # Now that everything is propagated, assert everyone agrees
    wait_for_cluster_propagation
}



test "Update human announced nodename and make sure they are all eventually propagated" {
    for {set j 0} {$j < [llength $::servers]} {incr j} {
        R $j config set cluster-announce-human-nodename "nodename-updated-$j.com"
    }
    wait_for_condition 50 100 {
        [are_nodenames_propagated "nodename-updated-*.com"] eq 1
    } else {
        fail "cluster human announced nodename were not propagated"
    }

    # Now that everything is propagated, assert everyone agrees
    wait_for_cluster_propagation
}


test "Remove human announced nodename and make sure they are all eventually propagated" {
    for {set j 0} {$j < [llength $::servers]} {incr j} {
        R $j config set cluster-announce-human-nodename ""
    }

    wait_for_condition 50 100 {
        [are_nodenames_propagated ""] eq 1
    } else {
        fail "cluster human announced nodename were not propagated"
    }

    # Now that everything is propagated, assert everyone agrees
    wait_for_cluster_propagation
}

test "Test restart will keep nodename information" {
    # Set a new nodename, reboot and make sure it sticks
    R 0 config set cluster-announce-human-nodename "restart-1.com"

    # Store the nodename in the config
    R 0 config rewrite

    restart_server 0 true false
    foreach n [get_cluster_nodes 0] {
        if { [string match [R 0 CLUSTER MYID] [dict get $n id]] } {
            set node $n
        }
    }
    wait_for_condition 50 100 {
        [dict get $node nodename] eq "restart-1.com"
    } else {
        fail "cluster human announced nodename were not propagated after restart"
    }
    # As a sanity check, make sure everyone eventually agrees
    wait_for_cluster_propagation
}

test "Test human announced nodename validation" {
    catch {R 0 config set cluster-announce-human-nodename "?.com"} err
    assert_match "* may only contain alphanumeric characters, hyphens, dots or underscore*" $err

    # Note this isn't a valid nodename, but it passes our internal validation
    R 0 config set cluster-announce-human-nodename "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-."
}
}
