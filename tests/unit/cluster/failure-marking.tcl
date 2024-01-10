# Test a single primary can mark replica as `fail`
start_cluster 1 1 {tags {external:skip cluster}} {

    test "Verify that single primary marks replica as failed" {
        set primary [srv -0 client]

        set replica1 [srv -1 client]
        set replica1_pid [srv -1 pid]
        set replica1_instance_id [dict get [cluster_get_myself 1] id]

        assert {[lindex [$primary role] 0] eq {master}}
        assert {[lindex [$replica1 role] 0] eq {slave}}

        wait_for_sync $replica1

        pause_process $replica1_pid

        wait_node_marked_fail 0 $replica1_instance_id
    }
}

# Test multiple primaries wait for a quorum and then mark a replica as `fail`
start_cluster 2 1 {tags {external:skip cluster}} {

    test "Verify that multiple primaries mark replica as failed" {
        set primary1 [srv -0 client]

        set primary2 [srv -1 client]
        set primary2_pid [srv -1 pid]

        set replica1 [srv -2 client]
        set replica1_pid [srv -2 pid]
        set replica1_instance_id [dict get [cluster_get_myself 2] id]

        assert {[lindex [$primary1 role] 0] eq {master}}
        assert {[lindex [$primary2 role] 0] eq {master}}
        assert {[lindex [$replica1 role] 0] eq {slave}}

        wait_for_sync $replica1

        pause_process $replica1_pid

        # Pause other primary to allow time for pfail flag to appear
        pause_process $primary2_pid

        wait_node_marked_pfail 0 $replica1_instance_id

        # Resume other primary and wait for to show replica as failed
        resume_process $primary2_pid

        wait_node_marked_fail 0 $replica1_instance_id
    }
}
