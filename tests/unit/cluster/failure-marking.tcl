proc check_node_marked_fail {ref_node_index instance_id_to_check} {
    set nodes [get_cluster_nodes $ref_node_index]

    foreach n $nodes {
        if {[dict get $n id] eq $instance_id_to_check} {
            return [cluster_has_flag $n fail]
        }
    }
    fail "Unable to find instance id in cluster nodes. ID: $instance_id_to_check"
}

proc check_node_marked_pfail {ref_node_index instance_id_to_check} {
    set nodes [get_cluster_nodes $ref_node_index]

    foreach n $nodes {
        if {[dict get $n id] eq $instance_id_to_check} {
            return [cluster_has_flag $n fail?]
        }
    }
    fail "Unable to find instance id in cluster nodes. ID: $instance_id_to_check"
}

# Test a single primary can fail a replica
start_cluster 1 1 {tags {external:skip cluster}} {

    test "Verify that single primary marks replica as failed" {
        assert {[s -0 role] eq {master}}
        assert {[s -1 role] eq {slave}}
        
        wait_for_condition 1000 50 {
            [s -1 master_link_status] eq {up}
        } else {
            fail "Replica #1 master link status is not up"
        }

        # Kill replica node
        set replica1_instance_id [dict get [cluster_get_myself 1] id]
        set replica1_pid [srv -1 pid]
        pause_process $replica1_pid

        # Wait for primary to show replica as failed
        wait_for_condition 1000 50 {
            [check_node_marked_fail 0 $replica1_instance_id]
        } else {
            fail "Replica node never marked as FAIL"
        }
    }
}

# Test multiple primaries wait for a quorum and then fail a replica
start_cluster 2 1 {tags {external:skip cluster}} {

    test "Verify that multiple primaries mark replica as failed" {
        assert {[s -0 role] eq {master}}
        assert {[s -1 role] eq {master}}
        assert {[s -2 role] eq {slave}}

        wait_for_condition 1000 50 {
            [s -2 master_link_status] eq {up}
        } else {
            fail "Replica #1 master link status is not up"
        }

        # Kill replica node
        set replica1_instance_id [dict get [cluster_get_myself 2] id]
        set replica1_pid [srv -2 pid]
        pause_process $replica1_pid

        # Pause other primary to allow time for pfail flag to appear
        set primary2_pid [srv -1 pid]
        pause_process $primary2_pid

        wait_for_condition 1000 50 {
            [check_node_marked_pfail 0 $replica1_instance_id]
        } else {
            fail "Replica node never marked as PFAIL"
        }

        # Resume other primary and wait for to show replica as failed
        resume_process $primary2_pid
        wait_for_condition 1000 50 {
            [check_node_marked_fail 0 $replica1_instance_id]
        } else {
            fail "Replica node never marked as FAIL"
        }
    }
}
