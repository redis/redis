source "../tests/includes/init-tests.tcl"

proc cluster_allocate_mixedSlots {n} {
    set slot 16383
    while {$slot >= 0} {
        set node [expr {$slot % $n}]
        lappend slots_$node $slot
        incr slot -1
    }
    for {set j 0} {$j < $n} {incr j} {
        R $j cluster addslots {*}[set slots_${j}]
    }
}

proc create_cluster_with_mixedSlot {masters slaves} {
    cluster_allocate_mixedSlots $masters
    if {$slaves} {
        cluster_allocate_slaves $masters $slaves
    }
    assert_cluster_state ok
}

test "Create a 5 nodes cluster" {
    create_cluster_with_mixedSlot 5 15
}

test "Cluster is up" {
    assert_cluster_state ok
}

test "Cluster is writable" {
    cluster_write_test 0
}

test "Instance #5 is a slave" {
    assert {[RI 5 role] eq {slave}}
}

test "client do not break when cluster slot" {
    R 0 config set client-output-buffer-limit "normal 33554432 16777216 60"
    if { [catch {R 0 cluster slots}] } {
        fail "output overflow when cluster slots"
    }
}

test "client can handle keys with hash tag" {
    set cluster [redis_cluster 127.0.0.1:[get_instance_attrib redis 0 port]]
    $cluster set foo{tag} bar
    $cluster close
}

test "slot migration is valid from primary to another primary" {
    set startup_port [get_instance_attrib redis 0 port]
    set cluster [redis_cluster 127.0.0.1:$startup_port]
    set key order1
    set slot [$cluster cluster keyslot $key]
    array set nodefrom [$cluster masternode_for_slot $slot]
    array set nodeto [$cluster masternode_notfor_slot $slot]
    array set node3 [$cluster masternode_notfor_slot $slot]
    while {$node3(port) eq $nodeto(port)} {
        array set node3 [$cluster masternode_notfor_slot $slot]
    }

    # Subscribe to moved slot notifications
    set rd1 [redis_deferring_client_by_addr 127.0.0.1 $nodefrom(port)]
    set rd2 [redis_deferring_client_by_addr 127.0.0.1 $nodeto(port)]
    set rd3 [redis_deferring_client_by_addr 127.0.0.1 $node3(port)]
    assert_equal {1} [subscribe $rd1 {__redis__:moved}]
    assert_equal {1} [subscribe $rd2 {__redis__:moved}]
    assert_equal {1} [subscribe $rd3 {__redis__:moved}]

    # Move the slot
    assert_equal {OK} [$nodeto(link) cluster setslot $slot importing $nodefrom(id)]
    assert_equal {OK} [$nodefrom(link) cluster setslot $slot migrating $nodeto(id)]
    assert_equal {OK} [$nodefrom(link) cluster setslot $slot node $nodeto(id)]
    assert_equal {OK} [$nodeto(link) cluster setslot $slot node $nodeto(id)]

    # Check that we got the pubsub MOVED message from all nodes.
    set expect_content "MOVED $slot 127.0.0.1:$nodeto(port)"
    set expect_publish [list message __redis__:moved $expect_content]
    assert_equal $expect_publish [$rd1 read]
    assert_equal $expect_publish [$rd2 read]
    assert_equal $expect_publish [$rd3 read]
    $rd1 close
    $rd2 close
    $rd3 close
}

test "slot migration is invalid from primary to replica" {
    set cluster [redis_cluster 127.0.0.1:[get_instance_attrib redis 0 port]]
    set key order1
    set slot [$cluster cluster keyslot $key]
    array set nodefrom [$cluster masternode_for_slot $slot]

    # Get replica node serving slot.
    set replicanodeinfo [$cluster cluster replicas $nodefrom(id)]
    puts $replicanodeinfo
    set args [split $replicanodeinfo " "]
    set replicaid [lindex [split [lindex $args 0] \{] 1]
    puts $replicaid

    catch {[$nodefrom(link) cluster setslot $slot node $replicaid]} err
    assert_match "*Target node is not a master" $err
}

if {$::tls} {
    test {CLUSTER SLOTS from non-TLS client in TLS cluster} {
        set slots_tls [R 0 cluster slots]
        set host [get_instance_attrib redis 0 host]
        set plaintext_port [get_instance_attrib redis 0 plaintext-port]
        set client_plain [redis $host $plaintext_port 0 0]
        set slots_plain [$client_plain cluster slots]
        $client_plain close
        # Compare the ports in the first row
        assert_no_match [lindex $slots_tls 0 3 1] [lindex $slots_plain 0 3 1]
    }
}
