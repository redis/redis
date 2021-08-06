source "../tests/includes/init-tests.tcl"

test "Create a 2 nodes cluster" {
    create_cluster 2 2
}

test "Cluster is up" {
    assert_cluster_state ok
}

set cluster [redis_cluster 127.0.0.1:[get_instance_attrib redis 0 port]]

test "Migrate a slot, verify client terminates on primary serving the slot." {
    
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

test "Migrate a slot, verify client terminates on replica serving the slot." {

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