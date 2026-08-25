#
# Copyright (c) 2009-Present, Redis Ltd.
# All rights reserved.
#
# Copyright (c) 2024-present, Valkey contributors.
# All rights reserved.
#
# Licensed under your choice of (a) the Redis Source Available License 2.0
# (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
# GNU Affero General Public License v3 (AGPLv3).
#
# Portions of this file are available under BSD3 terms; see REDISCONTRIBUTIONS for more information.
#

if {[info command redis_cluster] eq {}} {
    source tests/support/cluster.tcl
}
if {[info command rediscli_tls_config] eq {}} {
    source tests/support/cli.tcl
}

# Cluster helper functions
proc config_set_all_nodes {keyword value} {
    for {set j 0} {$j < [llength $::servers]} {incr j} {
        R $j config set $keyword $value
    }
}

proc get_instance_id_by_port {type port} {
    for {set j 0} {$j < [llength $::servers]} {incr j} {
        if {[srv [expr {-1*$j}] port] == $port} {
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

# Helper function to attempt to have each node in a cluster meet each other.
proc join_nodes_in_cluster {} {
    set ids {}
    for {set id 0} {$id < [llength $::servers]} {incr id} {
        lappend ids $id
    }
    for {set j 0} {$j < [expr {[llength $ids]-1}]} {incr j} {
        set a [lindex $ids $j]
        set b [lindex $ids [expr {$j+1}]]
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

# Search the first node starting from ID $first that is not already a replica.
proc cluster_find_available_replica {first} {
    for {set id 0} {$id < [llength $::servers]} {incr id} {
        if {$id < $first} continue
        set me [cluster_get_myself $id]
        if {[dict get $me slaveof] eq {-}} {
            return $id
        }
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
    wait_for_cluster_state ok
    wait_for_condition 100 100 {
        [catch {exec src/redis-cli {*}[rediscli_tls_config "./tests"] --cluster check $addr} result] == 0
    } else {
        puts "redis-cli --cluster check returns non-zero exit code, output below:\n$result"
        fail "Cluster could not settle with configuration"
    }
}

# Normalize cluster slots configuration by sorting replicas by node ID
proc normalize_cluster_slots {slots_config} {
    set normalized {}
    foreach slot_range $slots_config {
        if {[llength $slot_range] <= 3} {
            lappend normalized $slot_range
        } else {
            # Sort replicas (index 3+) by node ID, keep start/end/master unchanged
            set replicas [lrange $slot_range 3 end]
            set sorted_replicas [lsort -index 2 $replicas]
            lappend normalized [concat [lrange $slot_range 0 2] $sorted_replicas]
        }
    }
    return $normalized
}

# Check if cluster configuration is consistent.
proc cluster_config_consistent {} {
    # Only the nodes that were given a role take part in the comparison. A
    # topology started with spare nodes has them sitting outside the cluster
    # config, and the legacy helper did not wait on them either.
    set instances [expr {$::cluster_master_nodes + $::cluster_replica_nodes}]
    for {set j 0} {$j < $instances} {incr j} {
        if {$j == 0} {
            set base_cfg [R $j cluster slots]
            set base_secret [R $j debug internal_secret]
            set normalized_base_cfg [normalize_cluster_slots $base_cfg]
        } else {
            set cfg [R $j cluster slots]
            set secret [R $j debug internal_secret]
            set normalized_cfg [normalize_cluster_slots $cfg]
            if {$normalized_cfg != $normalized_base_cfg || $secret != $base_secret} {
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

# Return 1 if all instances have no in-progress atomic slot migration (ASM)
# task and no running slot trim.
proc asm_all_instances_idle {total} {
    for {set i 0} {$i < $total} {incr i} {
        if {[CI $i cluster_slot_migration_active_tasks] != 0} { return 0 }
        if {[CI $i cluster_slot_migration_active_trim_running] != 0} { return 0 }
    }
    return 1
}

# Wait for all atomic slot migration (ASM) tasks to complete in the cluster.
# The cluster_state can be "ok" while an ASM task is still running (slots stay
# covered throughout the migration), so callers that need the migration to be
# fully settled must use this instead of relying on wait_for_cluster_state.
proc wait_for_asm_done {} {
    set total_instances [expr {$::cluster_master_nodes + $::cluster_replica_nodes}]

    wait_for_condition 3000 10 {
        [asm_all_instances_idle $total_instances] == 1
    } else {
        # Print the number of active tasks on each instance
        for {set i 0} {$i < $total_instances} {incr i} {
            set migration_count [CI $i cluster_slot_migration_active_tasks]
            set trim_count [CI $i cluster_slot_migration_active_trim_running]
            puts "Instance $i: migration_tasks=$migration_count, trim_tasks=$trim_count"
        }
        fail "ASM tasks did not complete on all instances"
    }
    # wait all nodes to reach the same cluster config after ASM
    wait_for_cluster_propagation
}

# Wait for cluster size to be consistent across nodes.
proc wait_for_cluster_size {cluster_size} {
    wait_for_condition 1000 50 {
        [cluster_size_consistent $cluster_size] eq 1
    } else {
        fail "cluster size did not reach a consistent size $cluster_size"
    }
}

proc cluster_secrets_consistent {{excluded_ids {}}} {
    set secrets {}
    for {set j 0} {$j < [llength $::servers]} {incr j} {
        if {[lsearch -exact $excluded_ids $j] >= 0} continue
        lappend secrets [R $j debug internal_secret]
    }
    expr {[llength [lsort -unique $secrets]] <= 1}
}

# Check that the available cluster nodes agree about "state", or raise an
# error. Tests that intentionally stop nodes can pass their instance IDs in
# excluded_ids.
proc wait_for_cluster_state {state {excluded_ids {}}} {
    for {set j 0} {$j < [llength $::servers]} {incr j} {
        if {[lsearch -exact $excluded_ids $j] >= 0} continue
        wait_for_condition 1000 50 {
            [CI $j cluster_state] eq $state
        } else {
            fail "Cluster node $j cluster_state:[CI $j cluster_state]"
        }
    }

    # The legacy cluster runner treated secret convergence as part of reaching
    # a cluster state. Preserve that guarantee for the available nodes.
    wait_for_condition 50 100 {
        [cluster_secrets_consistent $excluded_ids]
    } else {
        fail "Failed waiting for cluster secrets to sync"
    }
}

# Stop and restart cluster nodes while keeping their normal-framework server
# definitions in place. Instance ID 0 is the innermost server, ID 1 is the
# next outer server, and so on.
proc cluster_kill_node {id} {
    set level [expr {-$id}]
    set server [get_srv $level]
    set pid [dict get $server pid]
    if {![is_alive $pid]} {
        error "Cluster node $id is already stopped"
    }
    kill_server $server
    # kill_server receives the server dictionary by value and closes its
    # client. Remove that now-invalid handle from the copy retained by the
    # server stack so restart and cleanup cannot close it a second time.
    dict unset server client
    lset ::servers end+$level $server
}

proc cluster_restart_node {id} {
    set level [expr {-$id}]
    set pid [srv $level pid]
    if {[is_alive $pid]} {
        error "Cluster node $id is still running"
    }
    restart_server $level true false
}

# Wait until redis-cli agrees that the cluster configuration is stable.
proc wait_for_cluster_stable {{id 0}} {
    set addr "127.0.0.1:[srv [expr {-$id}] port]"
    wait_for_condition 1000 50 {
        [catch {
            exec src/redis-cli --cluster check $addr \
                {*}[rediscli_tls_config "./tests"]
        }] == 0
    } else {
        fail "Cluster does not stabilize"
    }
}

# Default slot allocation for clusters, each master has a continuous block
# and approximately equal number of slots.
proc continuous_slot_allocation {masters {replicas {}}} {
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
proc cluster_allocate_slots {masters {replicas {}}} {
    set slot 16383
    while {$slot >= 0} {
        set node [randomInt $masters]
        lappend slots_$node $slot
        incr slot -1
    }
    for {set j 0} {$j < $masters} {incr j} {
        R $j cluster addslots {*}[set slots_${j}]
    }
}

proc default_replica_allocation {masters replicas} {
    set node_count [expr {$masters + $replicas}]
    for {set i 0} {$i < $masters} {incr i} {
        set nodeid [R $i CLUSTER MYID]
        for {set j [expr {$i + $masters}]} {$j < $node_count} {incr j $masters} {
            R $j CLUSTER REPLICATE $nodeid
        }
    }
}

# Add 'replicas' replicas to a cluster composed of 'masters' masters.
# It assumes that masters are allocated sequentially from instance ID 0 to N-1.
proc cluster_allocate_replicas {masters replicas} {
    for {set j 0} {$j < $replicas} {incr j} {
        set master_id [expr {$j % $masters}]
        set replica_id [cluster_find_available_replica $masters]
        set master_myself [cluster_get_myself $master_id]
        R $replica_id cluster replicate [dict get $master_myself id]
    }
}

proc cluster_call_slot_allocator {slot_allocator masters replicas} {
    set allocator_args [info args $slot_allocator]
    if {[lsearch -exact $allocator_args args] >= 0 || [llength $allocator_args] >= 2} {
        $slot_allocator $masters $replicas
    } else {
        $slot_allocator $masters
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

    cluster_call_slot_allocator $slot_allocator $masters $replicas

    wait_for_cluster_propagation

    # Setup master/replica relationships
    $replica_allocator $masters $replicas

    wait_for_cluster_propagation
    wait_for_cluster_state "ok"

    uplevel 1 $code
}

# Start a cluster with the given number of masters and replicas. Replicas
# will be allocated to masters by round robin. node_count is optional and may
# be larger than masters + replicas when a test needs unassigned/spare nodes.
proc start_cluster {masters replicas options code {slot_allocator continuous_slot_allocation} {replica_allocator default_replica_allocation} {node_count {}}} {
    set ::cluster_master_nodes $masters
    set ::cluster_replica_nodes $replicas
    set configured_node_count [expr {$masters + $replicas}]
    if {$node_count eq {}} {
        set node_count $configured_node_count
    } elseif {$node_count < $configured_node_count} {
        error "node_count ($node_count) must be at least masters + replicas ($configured_node_count)"
    }

    # Set the final code to be the tests + cluster setup
    set code [list cluster_setup $masters $replicas $node_count $slot_allocator $replica_allocator $code]

    # Configure the starting of multiple servers. Set cluster node timeout
    # aggressively since many tests depend on ping/pong messages. 
    set cluster_options [list overrides [list cluster-enabled yes cluster-ping-interval 100 cluster-node-timeout 3000 cluster-slot-stats-enabled yes]]
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

# Get a specific node by ID by parsing the CLUSTER NODES output.
proc cluster_get_node_by_id {instance_id node_id} {
    set nodes [get_cluster_nodes $instance_id]
    foreach n $nodes {
        if {[dict get $n id] eq $node_id} {
            return $n
        }
    }
    return {}
}

# Returns a parsed CLUSTER NODES output as a list of dictionaries. Optional
# status field can be specified to only return entries matching that status.
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

# Build the 2256-byte cluster bus header (CLUSTERMSG_MIN_LEN) shared by all
# message types. The sender identity, type, length, and the
# extensions/flags/mflags0 fields are supplied by the caller; everything else
# (epochs, slots, etc.) is fixed boilerplate. The extensions/flags/mflags0
# fields default to 0 for callers that don't need them (e.g. a PING carrying
# extension data overrides them). Message-type-specific payload is appended
# after this header.
proc build_cluster_bus_header {sender_name sender_port sender_cport msg_type totlen {num_extensions 0} {flags 0} {mflags0 0}} {
    set CLUSTER_NAMELEN 40
    set NET_IP_STR_LEN 46

    set sender_padded [binary format a${CLUSTER_NAMELEN} $sender_name]
    set myslots [string repeat \x00 [expr {16384/8}]]
    set slaveof [string repeat \x00 $CLUSTER_NAMELEN]
    set myip [string repeat \x00 $NET_IP_STR_LEN]
    set notused1 [string repeat \x00 30]

    set hdr ""
    append hdr "RCmb"
    append hdr [binary format I $totlen]
    append hdr [binary format S 1]                  ;# ver
    append hdr [binary format S $sender_port]       ;# port
    append hdr [binary format S $msg_type]          ;# type
    append hdr [binary format S 0]                  ;# count
    append hdr [binary format W 1]                  ;# currentEpoch
    append hdr [binary format W 2]                  ;# configEpoch
    append hdr [binary format W 0]                  ;# offset
    append hdr $sender_padded                       ;# sender
    append hdr $myslots                             ;# myslots
    append hdr $slaveof                             ;# slaveof
    append hdr $myip                                ;# myip
    append hdr [binary format S $num_extensions]    ;# extensions
    append hdr $notused1                            ;# notused1
    append hdr [binary format S 0]                  ;# pport
    append hdr [binary format S $sender_cport]      ;# cport
    append hdr [binary format S $flags]             ;# flags
    append hdr [binary format c 0]                  ;# state
    append hdr [binary format ccc $mflags0 0 0]     ;# mflags

    return $hdr
}
