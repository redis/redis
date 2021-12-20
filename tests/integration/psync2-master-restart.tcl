start_server {tags {"psync2 external:skip"}} {
start_server {} {
start_server {} {
    set master [srv 0 client]
    set master_host [srv 0 host]
    set master_port [srv 0 port]

    set replica [srv -1 client]
    set replica_host [srv -1 host]
    set replica_port [srv -1 port]

    set sub_replica [srv -2 client]

    # Build replication chain
    $replica replicaof $master_host $master_port
    $sub_replica replicaof $replica_host $replica_port

    wait_for_condition 50 100 {
        [status $replica master_link_status] eq {up} &&
        [status $sub_replica master_link_status] eq {up}
    } else {
        fail "Replication not started."
    }

    # Avoid PINGs
    $master config set repl-ping-replica-period 3600
    $master config rewrite

    # Generate some data
    createComplexDataset $master 1000

    test "PSYNC2: Partial resync after Master restart using RDB aux fields" {
        wait_for_condition 500 100 {
            [status $master master_repl_offset] == [status $replica master_repl_offset] &&
            [status $master master_repl_offset] == [status $sub_replica master_repl_offset]
        } else {
            fail "Replicas and master offsets were unable to match *exactly*."
        }

        set replid [status $master master_replid]
        set offset [status $master master_repl_offset]
        $replica config resetstat

        catch {
            # SHUTDOWN NOW ensures master doesn't send GETACK to replicas before
            # shutting down which would affect the replication offset.
            restart_server 0 true false true now
            set master [srv 0 client]
        }
        wait_for_condition 50 1000 {
            [status $replica master_link_status] eq {up} &&
            [status $sub_replica master_link_status] eq {up}
        } else {
            fail "Replicas didn't sync after master restart"
        }

        # Make sure master restore replication info correctly
        assert {[status $master master_replid] != $replid}
        assert {[status $master master_repl_offset] == $offset}
        assert {[status $master master_replid2] eq $replid}
        assert {[status $master second_repl_offset] == [expr $offset+1]}

        # Make sure master set replication backlog correctly
        assert {[status $master repl_backlog_active] == 1}
        assert {[status $master repl_backlog_first_byte_offset] == [expr $offset+1]}
        assert {[status $master repl_backlog_histlen] == 0}

        # Partial resync after Master restart
        assert {[status $master sync_partial_ok] == 1}
        assert {[status $replica sync_partial_ok] == 1}
    }

    test "PSYNC2: Partial resync after Master restart using RDB aux fields with expire" {
        $master debug set-active-expire 0
        for {set j 0} {$j < 1024} {incr j} {
            $master select [expr $j%16]
            $master set $j somevalue px 10
        }

        after 20

        # Wait until master has received ACK from replica. If the master thinks
        # that any replica is lagging when it shuts down, master would send
        # GETACK to the replicas, affecting the replication offset.
        set offset [status $master master_repl_offset]
        wait_for_condition 500 100 {
            [string match "*slave0:*,offset=$offset,*" [$master info replication]] &&
            $offset == [status $replica master_repl_offset] &&
            $offset == [status $sub_replica master_repl_offset]
        } else {
            show_cluster_status
            fail "Replicas and master offsets were unable to match *exactly*."
        }

        set offset [status $master master_repl_offset]
        $replica config resetstat

        catch {
            # Unlike the test above, here we use SIGTERM, which behaves
            # differently compared to SHUTDOWN NOW if there are lagging
            # replicas. This is just to increase coverage and let each test use
            # a different shutdown approach. In this case there are no lagging
            # replicas though.
            restart_server 0 true false
            set master [srv 0 client]
        }
        wait_for_condition 50 1000 {
            [status $replica master_link_status] eq {up} &&
            [status $sub_replica master_link_status] eq {up}
        } else {
            fail "Replicas didn't sync after master restart"
        }

        set expired_offset [status $master repl_backlog_histlen]
        # Stale keys expired and master_repl_offset grows correctly
        assert {[status $master rdb_last_load_keys_expired] == 1024}
        assert {[status $master master_repl_offset] == [expr $offset+$expired_offset]}

        # Partial resync after Master restart
        assert {[status $master sync_partial_ok] == 1}
        assert {[status $replica sync_partial_ok] == 1}

        set digest [$master debug digest]
        assert {$digest eq [$replica debug digest]}
        assert {$digest eq [$sub_replica debug digest]}
    }

    test "PSYNC2: Full resync after Master restart when too many key expired" {
        $master config set repl-backlog-size 16384
        $master config rewrite

        $master debug set-active-expire 0
        # Make sure replication backlog is full and will be trimmed.
        for {set j 0} {$j < 2048} {incr j} {
            $master select [expr $j%16]
            $master set $j somevalue px 10
        }

        after 20

        wait_for_condition 500 100 {
            [status $master master_repl_offset] == [status $replica master_repl_offset] &&
            [status $master master_repl_offset] == [status $sub_replica master_repl_offset]
        } else {
            fail "Replicas and master offsets were unable to match *exactly*."
        }

        $replica config resetstat

        catch {
            # Unlike the test above, here we use SIGTERM. This is just to
            # increase coverage and let each test use a different shutdown
            # approach.
            restart_server 0 true false
            set master [srv 0 client]
        }
        wait_for_condition 50 1000 {
            [status $replica master_link_status] eq {up} &&
            [status $sub_replica master_link_status] eq {up}
        } else {
            fail "Replicas didn't sync after master restart"
        }

        # Replication backlog is full
        assert {[status $master repl_backlog_first_byte_offset] > [status $master second_repl_offset]}
        assert {[status $master sync_partial_ok] == 0}
        assert {[status $master sync_full] == 1}
        assert {[status $master rdb_last_load_keys_expired] == 2048}
        assert {[status $replica sync_full] == 1}

        set digest [$master debug digest]
        assert {$digest eq [$replica debug digest]}
        assert {$digest eq [$sub_replica debug digest]}
    }
}}}
