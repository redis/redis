# Cluster helper functions

source tests/support/cluster.tcl

proc config_set_all_nodes {keyword value} {
    for {set j 0} {$j < [llength $::servers]} {incr j} {
        R $j config set $keyword $value
    }
}

proc get_instance_id_by_port {type port} {
    for {set j 0} {$j < [llength $::servers]} {incr j} {
        if {[srv [expr -1*$j] port] == $port} {
            return $j
        }
    }
    fail "Instance port $port not found."
}

# Check if the cluster is writable and readable. Use node "port"
# as a starting point to talk with the cluster.
proc cluster_write_test {port} {
    set prefix [randstring 20 20 alpha]
    set cluster [redis_cluster 127.0.0.1:$port]
    for {set j 0} {$j < 100} {incr j} {
        $cluster set key.$j $prefix.$j
    }
    for {set j 0} {$j < 100} {incr j} {
        assert {[$cluster get key.$j] eq "$prefix.$j"}
    }
    $cluster close
}

# Helper function to attempt to have each node in a cluster
# meet each other.
proc join_nodes_in_cluster {} {
    # Join node 0 with 1, 1 with 2, ... and so forth.
    # If auto-discovery works all nodes will know every other node
    # eventually.
    set ids {}
    for {set id 0} {$id < [llength $::servers]} {incr id} {lappend ids $id}
    for {set j 0} {$j < [expr [llength $ids]-1]} {incr j} {
        set a [lindex $ids $j]
        set b [lindex $ids [expr $j+1]]
        set b_port [srv -$b port]
        R $a cluster meet 127.0.0.1 $b_port
    }

    for {set id 0} {$id < [llength $::servers]} {incr id} {
        wait_for_condition 1000 50 {
            [llength [get_cluster_nodes $id connected]] == [llength $ids]
        } else {
            return 0
        }
    }
    return 1
}

# Search the first node starting from ID $first that is not
# already configured as a replica.
proc cluster_find_available_replica {first} {
    for {set id 0} {$id < [llength $::servers]} {incr id} {
        if {$id < $first} continue
        set me [cluster_get_myself $id]
        if {[dict get $me slaveof] eq {-}} {return $id}
    }
    fail "No available replicas"
}

proc fix_cluster {addr} {
    set code [catch {
        exec src/redis-cli {*}[rediscli_tls_config "./tests"] --cluster fix $addr << yes
    } result]
    if {$code != 0} {
        puts "redis-cli --cluster fix returns non-zero exit code, output below:\n$result"
    }
    # Note: redis-cli --cluster fix may return a non-zero exit code if nodes don't agree,
    # but we can ignore that and rely on the check below.
    wait_for_cluster_state ok
    wait_for_condition 100 100 {
        [catch {exec src/redis-cli {*}[rediscli_tls_config "./tests"] --cluster check $addr} result] == 0
    } else {
        puts "redis-cli --cluster check returns non-zero exit code, output below:\n$result"
        fail "Cluster could not settle with configuration"
    }
}

# Check if cluster configuration is consistent.
proc cluster_config_consistent {} {
    for {set j 0} {$j < [llength $::servers]} {incr j} {
        if {$j == 0} {
            set base_cfg [R $j cluster slots]
        } else {
            if {[R $j cluster slots] != $base_cfg} {
                return 0
            }
        }
    }

    return 1
}

# Check if cluster size is consistent.
proc cluster_size_consistent {cluster_size} {
    for {set j 0} {$j < $cluster_size} {incr j} {
        if {[CI $j cluster_known_nodes] ne $cluster_size} {
            return 0
        }
    }
    return 1
}

# Wait for cluster configuration to propagate and be consistent across nodes.
proc wait_for_cluster_propagation {} {
    wait_for_condition 500 100 {
        [cluster_config_consistent] eq 1
    } else {
        fail "cluster config did not reach a consistent state"
    }
}

# Wait for cluster size to be consistent across nodes.
proc wait_for_cluster_size {cluster_size} {
    wait_for_condition 1000 50 {
        [cluster_size_consistent $cluster_size] eq 1
    } else {
        fail "cluster size did not reach a consistent size $cluster_size"
    }
}

# Check that cluster nodes agree about "state", or raise an error.
proc wait_for_cluster_state {state} {
    for {set j 0} {$j < [llength $::servers]} {incr j} {
        wait_for_condition 1000 50 {
            [CI $j cluster_state] eq $state
        } else {
            fail "Cluster node $j cluster_state:[CI $j cluster_state]"
        }
    }
}

# Default slot allocation for clusters, each master has a continuous block
# and approximately equal number of slots.
proc continuous_slot_allocation {masters replicas} {
    set avg [expr double(16384) / $masters]
    set slot_start 0
    for {set j 0} {$j < $masters} {incr j} {
        set slot_end [expr int(ceil(($j + 1) * $avg) - 1)]
        R $j cluster addslotsrange $slot_start $slot_end
        set slot_start [expr $slot_end + 1]
    }
}

# Assuming nodes are reset, this function performs slots allocation.
# Only the first 'masters' nodes are used.
proc cluster_allocate_slots {masters replicas} {
    set slot 16383
    while {$slot >= 0} {
        # Allocate successive slots to random nodes.
        set node [randomInt $masters]
        lappend slots_$node $slot
        incr slot -1
    }
    for {set j 0} {$j < $masters} {incr j} {
        R $j cluster addslots {*}[set slots_${j}]
    }
}

proc default_replica_allocation {masters replicas} {
    # Setup master/replica relationships
    set node_count [expr $masters + $replicas]
    for {set i 0} {$i < $masters} {incr i} {
        set nodeid [R $i CLUSTER MYID]
        for {set j [expr $i + $masters]} {$j < $node_count} {incr j $masters} {
            R $j CLUSTER REPLICATE $nodeid
        }
    }
}

# Add 'replicas' replicas to a cluster composed of 'masters' masters.
# It assumes that masters are allocated sequentially from instance ID 0
# to N-1.
proc cluster_allocate_replicas {masters replicas} {
    for {set j 0} {$j < $replicas} {incr j} {
        set master_id [expr {$j % $masters}]
        set replica_id [cluster_find_available_replica $masters]
        set master_myself [cluster_get_myself $master_id]
        R $replica_id cluster replicate [dict get $master_myself id]
    }
}

# Setup method to be executed to configure the cluster before the
# tests run.
proc cluster_setup {masters replicas node_count slot_allocator replica_allocator code} {
    # Have all nodes meet
    if {$::tls} {
        set tls_cluster [lindex [R 0 CONFIG GET tls-cluster] 1]
    }
    if {$::tls && !$tls_cluster} {
        for {set i 1} {$i < $node_count} {incr i} {
            R 0 CLUSTER MEET [srv -$i host] [srv -$i pport]
        }         
    } else {
        for {set i 1} {$i < $node_count} {incr i} {
            R 0 CLUSTER MEET [srv -$i host] [srv -$i port]
        }
    }

    $slot_allocator $masters $replicas

    wait_for_cluster_propagation

    # Setup master/replica relationships
    $replica_allocator $masters $replicas

    wait_for_cluster_propagation

    wait_for_cluster_state "ok"

    uplevel 1 $code
}

# Start a cluster with the given number of masters and replicas. Replicas
# will be allocated to masters by round robin.
proc start_cluster {masters replicas options code {slot_allocator continuous_slot_allocation} {replica_allocator default_replica_allocation}} {
    set node_count [expr $masters + $replicas]
    # Set the final code to be the tests + cluster setup
    set code [list cluster_setup $masters $replicas $node_count $slot_allocator $replica_allocator $code]

    # Configure the starting of multiple servers. Set cluster node timeout
    # aggressively since many tests depend on ping/pong messages. 
    set cluster_options [list overrides [list cluster-enabled yes cluster-ping-interval 100 cluster-node-timeout 3000]]
    set options [concat $cluster_options $options]

    # Cluster mode only supports a single database, so before executing the tests
    # it needs to be configured correctly and needs to be reset after the tests. 
    set old_singledb $::singledb
    set ::singledb 1
    start_multiple_servers $node_count $options $code
    set ::singledb $old_singledb
}

# Test node for flag.
proc cluster_has_flag {node flag} {
    expr {[lsearch -exact [dict get $node flags] $flag] != -1}
}

# Returns the parsed "myself" node entry as a dictionary.
proc cluster_get_myself id {
    set nodes [get_cluster_nodes $id]
    foreach n $nodes {
        if {[cluster_has_flag $n myself]} {return $n}
    }
    return {}
}

# Get a specific node by ID by parsing the CLUSTER NODES output
# of the instance Number 'instance_id'
proc cluster_get_node_by_id {instance_id node_id} {
    set nodes [get_cluster_nodes $instance_id]
    foreach n $nodes {
        if {[dict get $n id] eq $node_id} {return $n}
    }
    return {}
}

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

# Returns 1 if no node knows node_id, 0 if any node knows it.
proc node_is_forgotten {node_id} {
    for {set j 0} {$j < [llength $::servers]} {incr j} {
        set cluster_nodes [R $j CLUSTER NODES]
        if { [string match "*$node_id*" $cluster_nodes] } {
            return 0
        }
    }
    return 1
}

# Isolate a node from the cluster and give it a new nodeid
proc isolate_node {id} {
    set node_id [R $id CLUSTER MYID]
    R $id CLUSTER RESET HARD
    # Here we additionally test that CLUSTER FORGET propagates to all nodes.
    set other_id [expr $id == 0 ? 1 : 0]
    R $other_id CLUSTER FORGET $node_id
    wait_for_condition 50 100 {
        [node_is_forgotten $node_id]
    } else {
        fail "CLUSTER FORGET was not propagated to all nodes"
    }
}

# Check if cluster's view of hostnames is consistent
proc are_hostnames_propagated {match_string} {
    for {set j 0} {$j < [llength $::servers]} {incr j} {
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

proc wait_node_marked_fail {ref_node_index instance_id_to_check} {
    wait_for_condition 1000 50 {
        [check_cluster_node_mark fail $ref_node_index $instance_id_to_check]
    } else {
        fail "Replica node never marked as FAIL ('fail')"
    }
}

proc wait_node_marked_pfail {ref_node_index instance_id_to_check} {
    wait_for_condition 1000 50 {
        [check_cluster_node_mark fail\? $ref_node_index $instance_id_to_check]
    } else {
        fail "Replica node never marked as PFAIL ('fail?')"
    }
}

proc check_cluster_node_mark {flag ref_node_index instance_id_to_check} {
    set nodes [get_cluster_nodes $ref_node_index]

    foreach n $nodes {
        if {[dict get $n id] eq $instance_id_to_check} {
            return [cluster_has_flag $n $flag]
        }
    }
    fail "Unable to find instance id in cluster nodes. ID: $instance_id_to_check"
}
