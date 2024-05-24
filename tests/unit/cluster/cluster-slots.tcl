proc cluster_allocate_mixedSlots {masters replicas} {
    set slot 16383
    while {$slot >= 0} {
        set node [expr {$slot % $masters}]
        lappend slots_$node $slot
        incr slot -1
    }
    for {set j 0} {$j < $masters} {incr j} {
        R $j cluster addslots {*}[set slots_${j}]
    }
}

start_cluster 5 10 {tags {external:skip cluster}} {

test "Cluster is up" {
    wait_for_cluster_state ok
}

test "Cluster is writable" {
    cluster_write_test [srv 0 port]
}

test "Instance #5 is a slave" {
    assert {[s -5 role] eq {slave}}
}

test "client do not break when cluster slot" {
    R 0 config set client-output-buffer-limit "normal 33554432 16777216 60"
    if { [catch {R 0 cluster slots}] } {
        fail "output overflow when cluster slots"
    }
}

test "client can handle keys with hash tag" {
    set cluster [redis_cluster 127.0.0.1:[srv 0 port]]
    $cluster set foo{tag} bar
    $cluster close
}

test "slot migration is valid from primary to another primary" {
    set cluster [redis_cluster 127.0.0.1:[srv 0 port]]
    set key order1
    set slot [$cluster cluster keyslot $key]
    array set nodefrom [$cluster masternode_for_slot $slot]
    array set nodeto [$cluster masternode_notfor_slot $slot]

    assert_equal {OK} [$nodefrom(link) cluster setslot $slot node $nodeto(id)]
    assert_equal {OK} [$nodeto(link) cluster setslot $slot node $nodeto(id)]
}

test "slot migration is invalid from primary to replica" {
    set cluster [redis_cluster 127.0.0.1:[srv 0 port]]
    set key order1
    set slot [$cluster cluster keyslot $key]
    array set nodefrom [$cluster masternode_for_slot $slot]

    # Get replica node serving slot.
    set replicanodeinfo [$cluster cluster replicas $nodefrom(id)]
    set args [split $replicanodeinfo " "]
    set replicaid [lindex [split [lindex $args 0] \{] 1]

    catch {[$nodefrom(link) cluster setslot $slot node $replicaid]} err
    assert_match "*Target node is not a master" $err
}

proc count_bound_slots {n} {
     set slot_count 0
     foreach slot_range_mapping [$n cluster slots] {
         set start_slot [lindex $slot_range_mapping 0]
         set end_slot [lindex $slot_range_mapping 1]
         incr slot_count [expr $end_slot - $start_slot + 1]
     }
     return $slot_count
 }

 test "slot must be unbound on the owner when it is deleted" {
     set node0 [Rn 0]
     set node1 [Rn 1]
     assert {[count_bound_slots $node0] eq 16384}
     assert {[count_bound_slots $node1] eq 16384}

     set slot_to_delete 0
     # Delete
     $node0 CLUSTER DELSLOTS $slot_to_delete

     # Verify
     # The node that owns the slot must unbind the slot that was deleted
     wait_for_condition 1000 50 {
         [count_bound_slots $node0] == 16383
     } else {
         fail "Cluster slot deletion was not recorded on the node that owns the slot"
     }

     # We don't propagate slot deletion across all nodes in the cluster.
     # This can lead to extra redirect before the clients find out that the slot is unbound.
     wait_for_condition 1000 50 {
         [count_bound_slots $node1] == 16384
     } else {
         fail "Cluster slot deletion should not be propagated to all nodes in the cluster"
     }
 }

if {$::tls} {
    test {CLUSTER SLOTS from non-TLS client in TLS cluster} {
        set slots_tls [R 0 cluster slots]
        set host [srv 0 host]
        set plaintext_port [srv 0 pport]
        set client_plain [redis $host $plaintext_port 0 0]
        set slots_plain [$client_plain cluster slots]
        $client_plain close
        # Compare the ports in the first row
        assert_no_match [lindex $slots_tls 0 3 1] [lindex $slots_plain 0 3 1]
    }
}

} cluster_allocate_mixedSlots cluster_allocate_replicas ;# start_cluster
