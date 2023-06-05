# Tests for the response of slot migrations.

source "../tests/includes/init-tests.tcl"
source "../tests/includes/utils.tcl"

test "Create a 2 nodes cluster" {
    create_cluster 2 0
    config_set_all_nodes cluster-allow-replica-migration no
}

test "Cluster is up" {
    assert_cluster_state ok
}

set cluster [redis_cluster 127.0.0.1:[get_instance_attrib redis 0 port]]
if {$::tls} {
    set cluster_pport [redis_cluster 127.0.0.1:[get_instance_attrib redis 0 pport] 0]
} else {
    set cluster_pport [redis_cluster 127.0.0.1:[get_instance_attrib redis 0 pport] 1]
}
catch {unset nodefrom}
catch {unset nodeto}

$cluster refresh_nodes_map

test "Set many keys in the cluster" {
    for {set i 0} {$i < 5000} {incr i} {
        $cluster set $i $i
        assert { [$cluster get $i] eq $i }
    }
}

proc get_port_form_error {e} {
    set ip_port [lindex [split $e " "] 2]
    return [lindex [split $ip_port ":"] 1]
}

proc get_pport_by_port {port} {
    set id [get_instance_id_by_port redis $port]
    return [get_instance_attrib redis $id pport]
}

test "Test cluster responses during migration of slot x" {

    set slot 10
    array set nodefrom [$cluster masternode_for_slot $slot]
    array set nodeto [$cluster masternode_notfor_slot $slot]

    $nodeto(link) cluster setslot $slot importing $nodefrom(id)
    $nodefrom(link) cluster setslot $slot migrating $nodeto(id)

    # Get a key from that slot
    set key [$nodefrom(link) cluster GETKEYSINSLOT $slot "1"]

    # MOVED REPLY
    catch {$nodeto(link) set $key "newVal"} e_moved1
    assert_error "*MOVED*" $e_moved1

    # ASK REPLY
    catch {$nodefrom(link) set "abc{$key}" "newVal"} e_ask1
    assert_error "*ASK*" $e_ask1

    # UNSTABLE REPLY
    assert_error "*TRYAGAIN*" {$nodefrom(link) mset "a{$key}" "newVal" $key "newVal2"}

    # Connecting using another protocol
    array set nodefrom_pport [$cluster_pport masternode_for_slot $slot]
    array set nodeto_pport [$cluster_pport masternode_notfor_slot $slot]

    # MOVED REPLY
    catch {$nodeto_pport(link) set $key "newVal"} e_moved2
    assert_error "*MOVED*" $e_moved2

    # ASK REPLY
    catch {$nodefrom_pport(link) set "abc{$key}" "newVal"} e_ask2
    assert_error "*ASK*" $e_ask2

    # Compare MOVED and ASK error's port
    set port1 [get_port_form_error $e_moved1]
    set port2 [get_port_form_error $e_moved2]
    assert_not_equal $port1 $port2
    assert_equal $port1 $nodefrom(port)
    assert_equal $port2 [get_pport_by_port $nodefrom(port)]

    set port1 [get_port_form_error $e_ask1]
    set port2 [get_port_form_error $e_ask2]
    assert_not_equal $port1 $port2
    assert_equal $port1 $nodeto(port)
    assert_equal $port2 [get_pport_by_port $nodeto(port)]
}

config_set_all_nodes cluster-allow-replica-migration yes
