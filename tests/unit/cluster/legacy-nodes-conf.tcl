# Compare slots and membership independently of shard ordering and regenerated IDs.
proc normalized_cluster_shards_topology {reference} {
    set topology {}
    foreach shard [R $reference CLUSTER SHARDS] {
        set node_ids {}
        foreach node [dict get $shard nodes] {
            lappend node_ids [dict get $node id]
        }
        lappend topology [list [dict get $shard slots] [lsort $node_ids]]
    }
    return [lsort $topology]
}

proc cluster_shards_topology_matches {expected_topology} {
    for {set id 0} {$id < [llength $::servers]} {incr id} {
        if {[normalized_cluster_shards_topology $id] ne $expected_topology} {
            return 0
        }
    }
    return 1
}

# Emulate nodes.conf from before Redis persisted the shard-id auxiliary field.
proc remove_shard_ids_from_cluster_config {filename} {
    set fd [open $filename r]
    set contents [read $fd]
    close $fd

    set replacements [regsub -all {,shard-id=[[:xdigit:]]{40}} $contents {} contents]
    assert_morethan $replacements 0 "No shard-id fields found in $filename"

    set fd [open $filename w]
    puts -nonewline $fd $contents
    close $fd
}

# Keep primary/replica roles stable while stopping and restarting the whole cluster.
start_cluster 3 3 {tags {external:skip cluster} overrides {cluster-replica-no-failover yes}} {
    test "Loading a pre-shard-id nodes.conf preserves shard topology" {
        # Record the agreed topology before stopping any node. Shard IDs may be
        # regenerated on load, but slots and node membership must stay unchanged.
        wait_for_condition 1000 50 {
            [cluster_shards_topology_matches [normalized_cluster_shards_topology 0]]
        } else {
            fail "Cluster shard topology did not converge before restart"
        }
        set expected_topology [normalized_cluster_shards_topology 0]
        assert_equal 3 [llength $expected_topology]
        set node_count [llength $::servers]
        set cluster_config_files {}

        # Persist the topology and resolve config paths before shutting down.
        for {set id 0} {$id < $node_count} {incr id} {
            R $id cluster saveconfig
            set dir [lindex [R $id config get dir] 1]
            set filename [lindex [R $id config get cluster-config-file] 1]
            lappend cluster_config_files [file join $dir $filename]
        }

        # Stop every node before editing files so no live node can overwrite the
        # legacy configuration or supply persisted shard IDs to a restarted peer.
        for {set id 0} {$id < $node_count} {incr id} {
            catch {R $id shutdown nosave}
            wait_for_condition 100 50 {
                ![is_alive [srv -$id pid]]
            } else {
                fail "Node $id did not shut down"
            }
        }
        foreach filename $cluster_config_files {
            remove_shard_ids_from_cluster_config $filename
        }

        # Check the first node before its peers can repair its view through gossip.
        restart_server 0 true false
        assert_equal $expected_topology [normalized_cluster_shards_topology 0]
        for {set id 1} {$id < $node_count} {incr id} {
            restart_server -$id true false
        }

        # Every node must retain all shards and their exact slot/node membership.
        wait_for_cluster_state ok
        wait_for_condition 1000 50 {
            [cluster_shards_topology_matches $expected_topology]
        } else {
            fail "Cluster shard topology did not converge after loading pre-shard-id configuration"
        }

        # start_cluster assigns replica i+3 to primary i. New primary shard IDs
        # must be unique, and each replica must inherit its primary's shard ID.
        set primary_shard_ids {}
        for {set id 0} {$id < 3} {incr id} {
            set shard_id [R $id CLUSTER MYSHARDID]
            assert_equal 40 [string length $shard_id]
            assert_equal -1 [lsearch -exact $primary_shard_ids $shard_id]
            lappend primary_shard_ids $shard_id
            wait_for_condition 100 50 {
                [R $id CLUSTER MYSHARDID] eq [R [expr {$id + 3}] CLUSTER MYSHARDID]
            } else {
                fail "Replica did not inherit primary $id's shard ID"
            }
        }
    }
}
