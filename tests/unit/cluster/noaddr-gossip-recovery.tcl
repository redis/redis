#
# Copyright (C) 2014-Present, Redis Ltd.
# All Rights reserved.
#
# Licensed under your choice of (a) the Redis Source Available License 2.0
# (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
# GNU Affero General Public License v3 (AGPLv3).

# Gossip repair for peers stuck in NOADDR (e.g. after address confusion).

proc cluster_unit_get_node_by_id {instance_id node_id} {
    foreach n [get_cluster_nodes $instance_id] {
        if {[dict get $n id] eq $node_id} {
            return $n
        }
    }
    return {}
}

# Return 1 when instance_id no longer marks peer_id as NOADDR and has a localhost addr for it.
proc cluster_unit_noaddr_repaired {instance_id peer_id} {
    set n2 [cluster_unit_get_node_by_id $instance_id $peer_id]
    if {$n2 eq {}} {return 0}
    if {[cluster_has_flag $n2 noaddr]} {return 0}
    if {![string match {127.0.0.1:*} [dict get $n2 addr]]} {return 0}
    return 1
}

proc cluster_unit_rep_info_field {rid name} {
    foreach line [split [R $rid info replication] "\n\r"] {
        if {[string match "${name}:*" $line]} {
            set i [string first : $line]
            return [string trim [string range $line [expr {$i + 1}] end]]
        }
    }
    return {}
}

# Replica `rid`: peer master repaired, master_host matches `want_host`, link up.
proc cluster_unit_replica_master_recovered {rid master_id want_host} {
    if {![cluster_unit_noaddr_repaired $rid $master_id]} {return 0}
    if {[cluster_unit_rep_info_field $rid master_host] ne $want_host} {return 0}
    if {[cluster_unit_rep_info_field $rid master_link_status] ne {up}} {return 0}
    return 1
}

start_cluster 3 0 {tags {external:skip cluster needs:debug}} {
    test "NOADDR peer address is repaired from trusted gossip" {
        set id2 [R 2 CLUSTER MYID]
        set n2 [cluster_unit_get_node_by_id 0 $id2]
        assert {$n2 ne {}}
        assert_equal 0 [cluster_has_flag $n2 noaddr]
        assert_match {127.0.0.1:*} [dict get $n2 addr]

        R 0 DEBUG FORCE-NOADDR $id2

        set n2 [cluster_unit_get_node_by_id 0 $id2]
        assert_equal 1 [cluster_has_flag $n2 noaddr]

        wait_for_condition 200 50 {[cluster_unit_noaddr_repaired 0 $id2]} else {
            fail "NOADDR gossip repair did not restore peer address on node 0"
        }
    }
}

start_cluster 2 1 {tags {external:skip cluster needs:debug}} {
    test "NOADDR gossip repair updates replica replication master address" {
        wait_for_condition 50 100 {
            [cluster_unit_rep_info_field 2 master_link_status] eq {up}
        } else {
            fail "replica did not sync to master"
        }
        set master_id [R 0 CLUSTER MYID]
        set want_host [cluster_unit_rep_info_field 2 master_host]

        R 2 DEBUG FORCE-NOADDR $master_id

        wait_for_condition 200 50 \
            [format {[cluster_unit_replica_master_recovered 2 %s %s]} $master_id $want_host] else {
            fail "replica master_host or link not restored after gossip NOADDR repair"
        }
    }
}
