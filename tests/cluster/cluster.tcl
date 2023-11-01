# Cluster-specific test functions.
#
# Copyright (C) 2014 Salvatore Sanfilippo antirez@gmail.com
# This software is released under the BSD License. See the COPYING file for
# more information.

# Track cluster configuration as created by create_cluster below
set ::cluster_master_nodes 0
set ::cluster_replica_nodes 0

# Returns a parsed CLUSTER NODES output as a list of dictionaries. Optional status field
# can be specified to only returns entries that match the provided status.
proc get_cluster_nodes {id {status "*"}} {
    set lines [split [R $id cluster nodes] "\r\n"]
    set nodes {}
    foreach l $lines {
        set l [string trim $l]
        if {$l eq {}} continue
        set args [split $l]
        set node [dict create \
            id [lindex $args 0] \
            addr [lindex $args 1] \
            flags [split [lindex $args 2] ,] \
            slaveof [lindex $args 3] \
            ping_sent [lindex $args 4] \
            pong_recv [lindex $args 5] \
            config_epoch [lindex $args 6] \
            linkstate [lindex $args 7] \
            slots [lrange $args 8 end] \
        ]
        if {[string match $status [lindex $args 7]]} {
            lappend nodes $node
        }
    }
    return $nodes
}

# Test node for flag.
proc has_flag {node flag} {
    expr {[lsearch -exact [dict get $node flags] $flag] != -1}
}

# Returns the parsed myself node entry as a dictionary.
proc get_myself id {
    set nodes [get_cluster_nodes $id]
    foreach n $nodes {
        if {[has_flag $n myself]} {return $n}
    }
    return {}
}

# Get a specific node by ID by parsing the CLUSTER NODES output
# of the instance Number 'instance_id'
proc get_node_by_id {instance_id node_id} {
    set nodes [get_cluster_nodes $instance_id]
    foreach n $nodes {
        if {[dict get $n id] eq $node_id} {return $n}
    }
    return {}
}

# Return the value of the specified CLUSTER INFO field.
proc CI {n field} {
    get_info_field [R $n cluster info] $field
}

# Return the value of the specified INFO field.
proc s {n field} {
    get_info_field [R $n info] $field
}

# Assuming nodes are reset, this function performs slots allocation.
# Only the first 'n' nodes are used.
proc cluster_allocate_slots {n} {
    set slot 16383
    while {$slot >= 0} {
        # Allocate successive slots to random nodes.
        set node [randomInt $n]
        lappend slots_$node $slot
        incr slot -1
    }
    for {set j 0} {$j < $n} {incr j} {
        R $j cluster addslots {*}[set slots_${j}]
    }
}

# Check that cluster nodes agree about "state", or raise an error.
proc assert_cluster_state {state} {
    foreach_redis_id id {
        if {[instance_is_killed redis $id]} continue
        wait_for_condition 1000 50 {
            [CI $id cluster_state] eq $state
        } else {
            fail "Cluster node $id cluster_state:[CI $id cluster_state]"
        }
    }
}

# Search the first node starting from ID $first that is not
# already configured as a slave.
proc cluster_find_available_slave {first} {
    foreach_redis_id id {
        if {$id < $first} continue
        if {[instance_is_killed redis $id]} continue
        set me [get_myself $id]
        if {[dict get $me slaveof] eq {-}} {return $id}
    }
    fail "No available slaves"
}

# Add 'slaves' slaves to a cluster composed of 'masters' masters.
# It assumes that masters are allocated sequentially from instance ID 0
# to N-1.
proc cluster_allocate_slaves {masters slaves} {
    for {set j 0} {$j < $slaves} {incr j} {
        set master_id [expr {$j % $masters}]
        set slave_id [cluster_find_available_slave $masters]
        set master_myself [get_myself $master_id]
        R $slave_id cluster replicate [dict get $master_myself id]
    }
}

# Create a cluster composed of the specified number of masters and slaves.
proc create_cluster {masters slaves} {
    cluster_allocate_slots $masters
    if {$slaves} {
        cluster_allocate_slaves $masters $slaves
    }
    assert_cluster_state ok

    set ::cluster_master_nodes $masters
    set ::cluster_replica_nodes $slaves
}

proc cluster_allocate_with_continuous_slots {n} {
    set slot 16383
    set avg [expr ($slot+1) / $n]
    while {$slot >= 0} {
        set node [expr $slot/$avg >= $n ? $n-1 : $slot/$avg]
        lappend slots_$node $slot
        incr slot -1
    }
    for {set j 0} {$j < $n} {incr j} {
        R $j cluster addslots {*}[set slots_${j}]
    }
}

# Create a cluster composed of the specified number of masters and slaves,
# but with a continuous slot range. 
proc cluster_create_with_continuous_slots {masters slaves} {
    cluster_allocate_with_continuous_slots $masters
    if {$slaves} {
        cluster_allocate_slaves $masters $slaves
    }
    assert_cluster_state ok

    set ::cluster_master_nodes $masters
    set ::cluster_replica_nodes $slaves
}


# Set the cluster node-timeout to all the reachalbe nodes.
proc set_cluster_node_timeout {to} {
    foreach_redis_id id {
        catch {R $id CONFIG SET cluster-node-timeout $to}
    }
}

# Check if the cluster is writable and readable. Use node "id"
# as a starting point to talk with the cluster.
proc cluster_write_test {id} {
    set prefix [randstring 20 20 alpha]
    set port [get_instance_attrib redis $id port]
    set cluster [redis_cluster 127.0.0.1:$port]
    for {set j 0} {$j < 100} {incr j} {
        $cluster set key.$j $prefix.$j
    }
    for {set j 0} {$j < 100} {incr j} {
        assert {[$cluster get key.$j] eq "$prefix.$j"}
    }
    $cluster close
}

# Check if cluster configuration is consistent.
proc cluster_config_consistent {} {
    for {set j 0} {$j < $::cluster_master_nodes + $::cluster_replica_nodes} {incr j} {
        if {$j == 0} {
            set base_cfg [R $j cluster slots]
        } else {
            set cfg [R $j cluster slots]
            if {$cfg != $base_cfg} {
                return 0
            }
        }
    }

    return 1
}

# Wait for cluster configuration to propagate and be consistent across nodes.
proc wait_for_cluster_propagation {} {
    wait_for_condition 50 100 {
        [cluster_config_consistent] eq 1
    } else {
        fail "cluster config did not reach a consistent state"
    }
}

# Check if cluster's view of hostnames is consistent
proc are_hostnames_propagated {match_string} {
    for {set j 0} {$j < $::cluster_master_nodes + $::cluster_replica_nodes} {incr j} {
        set cfg [R $j cluster slots]
        foreach node $cfg {
            for {set i 2} {$i < [llength $node]} {incr i} {
                if {! [string match $match_string [lindex [lindex [lindex $node $i] 3] 1]] } {
                    return 0
                }
            }
        }
    }
    return 1
}
