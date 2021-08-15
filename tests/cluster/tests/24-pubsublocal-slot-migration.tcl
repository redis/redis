source "../tests/includes/init-tests.tcl"

proc cluster_allocate_linearslots {n} {
    set maxslot 16384
    set split [expr {$maxslot / $n + 1}]
    set increment $split
    set node 0
    for {set j 1} {$j <= $maxslot} {incr j} {
        if {$j == $split} {
            incr node
            incr split $increment
        }
        set slot [expr $j - 1]
        R $node cluster addslots $slot
    }
}

proc create_cluster_with_linearslots {primary replica} {
    cluster_allocate_linearslots $primary
    cluster_allocate_slaves $primary $replica
}

test "Create a 3 nodes cluster" {
    create_cluster_with_linearslots 3 3
}

test "Cluster is up" {
    assert_cluster_state ok
}

set cluster [redis_cluster 127.0.0.1:[get_instance_attrib redis 0 port]]

test "Migrate a slot, verify client receives unsubscribelocal on primary serving the slot." {

    # Setup the to and from node
    set channelname mychannel
    set slot [$cluster cluster keyslot $channelname]
    array set nodefrom [$cluster masternode_for_slot $slot]
    array set nodeto [$cluster masternode_notfor_slot $slot]

    set subscribeclient [redis_deferring_client $nodefrom(host) $nodefrom(port)]

    $subscribeclient deferred 1
    $subscribeclient subscribelocal $channelname
    $subscribeclient read

    assert_equal {OK} [$nodefrom(link) cluster setslot $slot migrating $nodeto(id)]
    assert_equal {OK} [$nodeto(link) cluster setslot $slot importing $nodefrom(id)]

    assert_equal {OK} [$nodeto(link) cluster setslot $slot node $nodeto(id)]
   
    set msg [$subscribeclient read]
    assert {"unsubscribelocal" eq [lindex $msg 0]}
    assert {$channelname eq [lindex $msg 1]}
    assert {"0" eq [lindex $msg 2]}

    assert_equal {OK} [$nodeto(link) cluster setslot $slot node $nodeto(id)]
    
    $subscribeclient close
}

test "Client subscribes to multiple channels, migrate a slot, verify client receives unsubscribelocal on primary serving the slot." {

    # Setup the to and from node
    set channelname ch3
    set anotherchannelname ch7
    set slot [$cluster cluster keyslot $channelname]
    array set nodefrom [$cluster masternode_for_slot $slot]
    array set nodeto [$cluster masternode_notfor_slot $slot]

    set subscribeclient [redis_deferring_client $nodefrom(host) $nodefrom(port)]

    $subscribeclient deferred 1
    $subscribeclient subscribelocal $channelname $anotherchannelname
    $subscribeclient read
    $subscribeclient read

    assert_equal {OK} [$nodefrom(link) cluster setslot $slot migrating $nodeto(id)]
    assert_equal {OK} [$nodeto(link) cluster setslot $slot importing $nodefrom(id)]

    assert_equal {OK} [$nodeto(link) cluster setslot $slot node $nodeto(id)]

    # Verify the client receives unsubscribelocal message for the channel(slot) which got migrated.
    set msg [$subscribeclient read]
    assert {"unsubscribelocal" eq [lindex $msg 0]}
    assert {$channelname eq [lindex $msg 1]}
    assert {"1" eq [lindex $msg 2]}

    assert_equal {OK} [$nodeto(link) cluster setslot $slot node $nodeto(id)]

    $nodefrom(link) publishlocal $anotherchannelname hello

    # Verify the client is still connected and receives message from the other channel.
    set msg [$subscribeclient read]
    assert {"message" eq [lindex $msg 0]}
    assert {$anotherchannelname eq [lindex $msg 1]}
    assert {"hello" eq [lindex $msg 2]}

    $subscribeclient close
}

test "Migrate a slot, verify client receives unsubscribelocal on replica serving the slot." {

    # Setup the to and from node
    set channelname mychannel1
    set slot [$cluster cluster keyslot $channelname]
    array set nodefrom [$cluster masternode_for_slot $slot]
    array set nodeto [$cluster masternode_notfor_slot $slot]

    # Get replica node serving slot (mychannel) to connect a client.
    set replicanodeinfo [$cluster cluster replicas $nodefrom(id)]
    set args [split $replicanodeinfo " "]
    set addr [lindex [split [lindex $args 1] @] 0]
    set replicahost [lindex [split $addr :] 0]
    set replicaport [lindex [split $addr :] 1]
    set subscribeclient [redis_deferring_client $replicahost $replicaport]

    $subscribeclient deferred 1
    $subscribeclient subscribelocal $channelname
    $subscribeclient read

    assert_equal {OK} [$nodefrom(link) cluster setslot $slot migrating $nodeto(id)]
    assert_equal {OK} [$nodeto(link) cluster setslot $slot importing $nodefrom(id)]

    assert_equal {OK} [$nodeto(link) cluster setslot $slot node $nodeto(id)]    
    assert_equal {OK} [$nodeto(link) cluster setslot $slot node $nodeto(id)]

    set msg [$subscribeclient read]
    assert {"unsubscribelocal" eq [lindex $msg 0]}
    assert {$channelname eq [lindex $msg 1]}
    assert {"0" eq [lindex $msg 2]}
    
    $cluster close
    $subscribeclient close
}