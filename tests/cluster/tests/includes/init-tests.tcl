# Initialization tests -- most units will start including this.

test "(init) Restart killed instances" {
    foreach type {redis} {
        foreach_${type}_id id {
            if {[get_instance_attrib $type $id pid] == -1} {
                puts -nonewline "$type/$id "
                flush stdout
                restart_instance $type $id
            }
        }
    }
}

test "Cluster nodes are reachable" {
    foreach_redis_id id {
        # Every node should be reachable.
        wait_for_condition 1000 50 {
            ([catch {R $id ping} ping_reply] == 0) &&
            ($ping_reply eq {PONG})
        } else {
            catch {R $id ping} err
            fail "Node #$id keeps replying '$err' to PING."
        }
    }
}

test "Cluster nodes hard reset" {
    if {$::valgrind} {
        set node_timeout 10000
    } else {
        set node_timeout 3000
    }

    # A reset node can learn stale topology from nodes that have not been
    # reset yet. Repeat until every node is an isolated master.
    for {set attempts 3} {$attempts > 0} {incr attempts -1} {
        foreach_redis_id id {
            # Wait until slave is synced. Otherwise, it may reply -LOADING
            # for any commands below.
            if {[RI $id role] eq {slave}} {
                wait_for_condition 50 1000 {
                    [RI $id master_link_status] eq {up}
                } else {
                    fail "Slave were not able to sync."
                }
            }

            # Make FLUSHALL executable regardless of whether the node is currently
            # a replica. Run it in the same transaction as CLUSTER RESET so a node
            # cannot be promoted with data in between the two commands.
            R $id config set replica-read-only no
            R $id MULTI
            R $id flushall
            R $id cluster reset hard
            R $id cluster set-config-epoch [expr {$id+1}]
            set reset_results [R $id EXEC]
            R $id config set replica-read-only yes
            assert_equal OK [lindex $reset_results 0]
            assert_equal OK [lindex $reset_results 1]
            assert_equal OK [lindex $reset_results 2]
            R $id config set cluster-node-timeout $node_timeout
            R $id config set cluster-slave-validity-factor 10
            R $id config set loading-process-events-interval-bytes 2097152
            R $id config set key-load-delay 0
            R $id config set repl-diskless-load disabled
            R $id config set cluster-announce-hostname ""
            R $id DEBUG DROP-CLUSTER-PACKET-FILTER -1
            R $id config rewrite
        }

        set reset_complete 1
        foreach_redis_id id {
            if {[RI $id role] ne {master} || [CI $id cluster_known_nodes] != 1} {
                set reset_complete 0
                break
            }
        }
        if {$reset_complete} {
            break
        }
    }
    if {!$reset_complete} {
        fail "Cluster nodes did not become isolated masters after hard reset"
    }
}

# Helper function to attempt to have each node in a cluster
# meet each other.
proc join_nodes_in_cluster {} {
    # Join node 0 with 1, 1 with 2, ... and so forth.
    # If auto-discovery works all nodes will know every other node
    # eventually.
    set ids {}
    foreach_redis_id id {lappend ids $id}
    for {set j 0} {$j < [expr [llength $ids]-1]} {incr j} {
        set a [lindex $ids $j]
        set b [lindex $ids [expr $j+1]]
        set b_port [get_instance_attrib redis $b port]
        R $a cluster meet 127.0.0.1 $b_port
    }

    foreach_redis_id id {
        wait_for_condition 1000 50 {
            [llength [get_cluster_nodes $id connected]] == [llength $ids]
        } else {
            return 0
        }
    }
    return 1
}

test "Cluster Join and auto-discovery test" {
    # Use multiple attempts since sometimes nodes timeout
    # while attempting to connect.
    for {set attempts 3} {$attempts > 0} {incr attempts -1} {
        if {[join_nodes_in_cluster] == 1} {
            break
        }
    }
    if {$attempts == 0} {
        fail "Cluster failed to form full mesh"
    }
}

test "Before slots allocation, all nodes report cluster failure" {
    assert_cluster_state fail
}
