# Check the basic monitoring and failover capabilities.

# make sure the test infra won't use SELECT
set old_singledb $::singledb
set ::singledb 1

tags {tls:skip external:skip cluster} {

set base_conf [list cluster-enabled yes]
start_multiple_servers 5 [list overrides $base_conf] {

test "Cluster nodes are reachable" {
    for {set id 0} {$id < [llength $::servers]} {incr id} {
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
    for {set id 0} {$id < [llength $::servers]} {incr id} {
        if {$::valgrind} {
            set node_timeout 10000
        } else {
            set node_timeout 3000
        }
        catch {R $id flushall} ; # May fail for readonly slaves.
        R $id MULTI
        R $id cluster reset hard
        R $id cluster set-config-epoch [expr {$id+1}]
        R $id EXEC
        R $id config set cluster-node-timeout $node_timeout
        R $id config set cluster-slave-validity-factor 10
        R $id config set loading-process-events-interval-bytes 2097152
        R $id config set key-load-delay 0
        R $id config set repl-diskless-load disabled
        R $id config set cluster-announce-hostname ""
        R $id DEBUG DROP-CLUSTER-PACKET-FILTER -1
        R $id config rewrite
    }
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
    wait_for_cluster_state fail
}

test "Different nodes have different IDs" {
    set ids {}
    set numnodes 0
    for {set id 0} {$id < [llength $::servers]} {incr id} {
        incr numnodes
        # Every node should just know itself.
        set nodeid [dict get [cluster_get_myself $id] id]
        assert {$nodeid ne {}}
        lappend ids $nodeid
    }
    set numids [llength [lsort -unique $ids]]
    assert {$numids == $numnodes}
}

test "It is possible to perform slot allocation" {
    cluster_allocate_slots 5 0
}

test "After the join, every node gets a different config epoch" {
    set trynum 60
    while {[incr trynum -1] != 0} {
        # We check that this condition is true for *all* the nodes.
        set ok 1 ; # Will be set to 0 every time a node is not ok.
        for {set id 0} {$id < [llength $::servers]} {incr id} {
            set epochs {}
            foreach n [get_cluster_nodes $id] {
                lappend epochs [dict get $n config_epoch]
            }
            if {[lsort $epochs] != [lsort -unique $epochs]} {
                set ok 0 ; # At least one collision!
            }
        }
        if {$ok} break
        after 1000
        puts -nonewline .
        flush stdout
    }
    if {$trynum == 0} {
        fail "Config epoch conflict resolution is not working."
    }
}

test "Nodes should report cluster_state is ok now" {
    wait_for_cluster_state ok
}

test "Sanity for CLUSTER COUNTKEYSINSLOT" {
    set reply [R 0 CLUSTER COUNTKEYSINSLOT 0]
    assert {$reply eq 0}
}

test "It is possible to write and read from the cluster" {
    cluster_write_test [srv 0 port]
}

test "CLUSTER RESET SOFT test" {
    set last_epoch_node0 [CI 0 cluster_current_epoch]
    R 0 FLUSHALL
    R 0 CLUSTER RESET
    assert {[CI 0 cluster_current_epoch] eq $last_epoch_node0}

    set last_epoch_node1 [CI 1 cluster_current_epoch]
    R 1 FLUSHALL
    R 1 CLUSTER RESET SOFT
    assert {[CI 1 cluster_current_epoch] eq $last_epoch_node1}
}

test "Coverage: CLUSTER HELP" {
    assert_match "*CLUSTER <subcommand> *" [R 0 CLUSTER HELP]
}

test "Coverage: ASKING" {
    assert_equal {OK} [R 0 ASKING]
}

test "CLUSTER SLAVES and CLUSTER REPLICAS with zero replicas" {
    assert_equal {} [R 0 cluster slaves [R 0 CLUSTER MYID]]
    assert_equal {} [R 0 cluster replicas [R 0 CLUSTER MYID]]
}

} ;# stop servers

} ;# tags

set ::singledb $old_singledb
