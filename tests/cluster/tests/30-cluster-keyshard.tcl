source "../tests/includes/init-tests.tcl"

# Create a cluster with 2 master and 2 slaves, so that we have 1
# slaves for each master.
test "Create a 2 nodes cluster" {
    create_cluster 2 2
}

test "Cluster is up" {
    assert_cluster_state ok
}

set cluster [redis_cluster 127.0.0.1:[get_instance_attrib redis 0 port]]

proc get_cluster_replicanodes {cluster id} {
    set lines [split [$cluster CLUSTER REPLICAS $id] "\r\n"]
    set nodes {}
    foreach l $lines {
        set l [string trim $l]
        if {$l eq {}} continue
        set args [split $l]
	set node [dict create \
		ip [lindex [split [lindex [split [lindex $args 1] @] 0] :] 0] \
		port [lindex [split [lindex [split [lindex $args 1] @] 0] :] 1] \
		shardid [lindex [split [lsearch -inline [split [lindex $args 1] ,] {shard-id=*}] =] 1] \
		]
	lappend nodes $node
    }
    return $nodes
}

proc get_cluster_keyshard {id key type} {
    set keyshard_response [R $id CLUSTER KEYSHARD $key]
    foreach keyshard_response $keyshard_response {
	set keyshard_nodes [dict get $keyshard_response nodes]
        set keyshard_master [lindex $keyshard_nodes 0]
        set keyshard_replica [lindex $keyshard_nodes 1]
    }
    if {$type eq "master"} {
        return $keyshard_master
    } elseif {$type eq "replica"} {
	return $keyshard_replica
    } elseif {$type eq "nodes"} {
        return $keyshard_nodes
    }
    return {}
}

test "cluster keyshard response in node with key slot and empty key" {
    set key key1
    set slot [$cluster cluster keyslot $key]
    array set nodefrom [$cluster masternode_for_slot $slot]
    #array set nodeto [$cluster masternode_notfor_slot $slot]
    set nodeid $nodefrom(id)
    for {set j 0} {$j < $::cluster_master_nodes + $::cluster_replica_nodes} {incr j} {
        set myid [R $j CLUSTER MYID]
	if {$myid == $nodeid} { set id $j}
    }
    set nodes [get_cluster_keyshard $id $key "nodes"]
    puts "nodes : $nodes"

    set nodelen [llength $nodes]
    assert_equal $nodelen 0
}

test "cluster keyshard response in node where key and key slot exist" {
    set key key1
    set slot [$cluster cluster keyslot $key]
    array set nodefrom [$cluster masternode_for_slot $slot]
    array set nodeto [$cluster masternode_notfor_slot $slot]
    set nodeid $nodefrom(id)
    for {set j 0} {$j < $::cluster_master_nodes + $::cluster_replica_nodes} {incr j} {
        set myid [R $j CLUSTER MYID]
	if {$myid == $nodeid} {
	    R $j SET $key $j
	    set id $j
	    break 
	}
    }
    set keyshard_master [get_cluster_keyshard $id $key "master"]
    set keyshard_replica [get_cluster_keyshard $id $key "replica"] 

    set replicas [get_cluster_replicanodes $cluster $nodeid]
    assert_equal "127.0.0.1" [dict get $keyshard_master ip]
    assert_equal "3000$id" [dict get $keyshard_master port]
    assert_equal [R $j cluster myshardid] [dict get $keyshard_master shard-id]
    assert_equal "master" [dict get $keyshard_master role] 
    foreach replica $replicas {
	assert_equal [dict get $replica ip] [dict get $keyshard_replica ip]
        assert_equal [dict get $replica port] [dict get $keyshard_replica port]
        assert_equal [dict get $replica shardid] [dict get $keyshard_replica shard-id]
	assert_equal "replica" [dict get $keyshard_replica role]
     }
}


test "cluster keyshard response in node where key slot doesn't exist" {
    set key key1
    set slot [$cluster cluster keyslot $key]
    array set nodefrom [$cluster masternode_for_slot $slot]
    array set nodeto [$cluster masternode_notfor_slot $slot]
    set nodefromid $nodefrom(id)
    set nodetoid $nodeto(id)
    for {set j 0} {$j < $::cluster_master_nodes + $::cluster_replica_nodes} {incr j} {
        set myid [R $j CLUSTER MYID]
        if {$myid == $nodefromid} {set p $j}
        if {$myid == $nodetoid} {set id $j}
    }
    set keyshard_master [get_cluster_keyshard $id $key "master"]
    set keyshard_replica [get_cluster_keyshard $id $key "replica"]
    set replicas [get_cluster_replicanodes $cluster $nodeid]
    assert_equal "127.0.0.1" [dict get $keyshard_master ip]
    assert_equal "3000$p" [dict get $keyshard_master port]
    assert_equal [R $p cluster myshardid] [dict get $keyshard_master shard-id]
    assert_equal "master" [dict get $keyshard_master role]
    foreach replica $replicas {
        assert_equal [dict get $replica ip] [dict get $keyshard_replica ip]
        assert_equal [dict get $replica port] [dict get $keyshard_replica port]
        assert_equal [dict get $replica shardid] [dict get $keyshard_replica shard-id]
	assert_equal "replica" [dict get $keyshard_replica role]
     }
}

