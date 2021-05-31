# Test PUBSUB local propagation in a cluster slot.

source "../tests/includes/init-tests.tcl"

test "Create a 5 nodes cluster" {
    create_cluster 5 5
}

test "Pub/Sub local basics" {

    set port [get_instance_attrib redis 0 port]
    set cluster [redis_cluster 127.0.0.1:$port]

    set slot [$cluster cluster keyslot "channel.0"]
    array set publishnode [$cluster masternode_for_slot $slot]
    array set notlocalnode [$cluster masternode_notfor_slot $slot]

    puts $publishnode(port)
    puts $notlocalnode(port)

    set publishclient [redis_deferring_client $publishnode(host) $publishnode(port)]
    set subscribeclient [redis_deferring_client $publishnode(host) $publishnode(port)]
    set subscribeclient2 [redis_deferring_client $publishnode(host) $publishnode(port)]
    set anotherclient [redis_deferring_client $notlocalnode(host) $notlocalnode(port)]

    $subscribeclient deferred 1
    $subscribeclient subscribelocal channel.0
    $subscribeclient read

    $subscribeclient2 deferred 1
    $subscribeclient2 subscribelocal channel.0
    $subscribeclient2 read

    catch {$anotherclient subscribelocal channel.0} err
    puts [string range $err 0 4]
    #assert {[string range $err 0 4] eq {MOVED}}

    set data [randomValue]
    $publishclient publishlocal channel.0 $data

    set msg [$subscribeclient read]
    assert {$data eq [lindex $msg 2]}

    set msg [$subscribeclient2 read]
    assert {$data eq [lindex $msg 2]}

    $cluster close
    $publishclient close
    $subscribeclient close
    $subscribeclient2 close
    $anotherclient close
}

test "client can't subscribe to multiple local channels across different slots" {
    set port [get_instance_attrib redis 0 port]
    set cluster [redis_cluster 127.0.0.1:$port]

    catch {$cluster subscribelocal channel.0 channel.1} err

    assert_match {CROSSSLOT Channels*} $err
}

test "Verify Pub/Sub and Pub/Sub local no overlap" {

    set port [get_instance_attrib redis 0 port]
    set cluster [redis_cluster 127.0.0.1:$port]

    set slot [$cluster cluster keyslot "channel.0"]
    array set publishnode [$cluster masternode_for_slot $slot]
    array set notlocalnode [$cluster masternode_notfor_slot $slot]

    set publishlocalclient [redis_deferring_client $publishnode(host) $publishnode(port)]
    set publishclient [redis_deferring_client $publishnode(host) $publishnode(port)]
    set subscribeclientlocal [redis_deferring_client $publishnode(host) $publishnode(port)]
    set subscribeclient [redis_deferring_client $publishnode(host) $publishnode(port)]

    $subscribeclientlocal deferred 1
    $subscribeclientlocal subscribelocal channel.0
    $subscribeclientlocal read

    $subscribeclient deferred 1
    $subscribeclient subscribe channel.0
    $subscribeclient read

    set localdata "inGodWeTrust"
    $publishlocalclient publishlocal channel.0 $localdata

    set data "restBringData"
    $publishclient publish channel.0 $data

    set msg [$subscribeclientlocal read]
    assert {$localdata eq [lindex $msg 2]}

    set msg [$subscribeclient read]
    assert {$data eq [lindex $msg 2]}

    $cluster close
    $publishclient close
    $subscribeclient close
    $subscribeclientlocal close
}