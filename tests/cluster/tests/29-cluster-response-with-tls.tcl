# This test must be executed last

proc get_port_from_moved_error {e} {
    set ip_port [lindex [split $e " "] 2]
    return [lindex [split $ip_port ":"] 1]
}

proc get_pport_by_port {port} {
    set id [get_instance_id_by_port redis $port]
    return [get_instance_attrib redis $id pport]
}

proc get_port_form_node_info {line} {
    set fields [split $line " "]
    set addr [lindex $fields 1]
    set ip_port [lindex [split $addr "@"] 0]
    return [lindex [split $ip_port ":"] 1]
}

proc tls_service_tests {} {

    test "CLUSTER SLOTS with different connnection type" {
        set slots1 [R 0 cluster slots]
        set pport [get_instance_attrib redis 0 pport]
        set cluster_client [redis_cluster 127.0.0.1:$pport 0]
        set slots2 [$cluster_client cluster slots]
        $cluster_client close
        # Compare the ports in the first row
        assert_no_match [lindex $slots1 0 2 1] [lindex $slots2 0 2 1]
    }

    test "CLUSTER NODES return port according to connection type" {
        set nodes [R 0 cluster nodes]
        set port1 [get_port_form_node_info [lindex [split $nodes "\r\n"] 0]]
        set pport [get_instance_attrib redis 0 pport]
        set cluster_client [redis_cluster 127.0.0.1:$pport 0]
        set nodes [$cluster_client cluster nodes]
        set port2 [get_port_form_node_info [lindex [split $nodes "\r\n"] 0]]
        $cluster_client close
        assert_not_equal $port1 $port2
    }
    
    config_set_all_nodes cluster-allow-replica-migration no
    set cluster [redis_cluster 127.0.0.1:[get_instance_attrib redis 0 port]]
    set cluster_pport [redis_cluster 127.0.0.1:[get_instance_attrib redis 0 pport] 0]
    catch {unset nodefrom}
    catch {unset nodeto}
    $cluster refresh_nodes_map

    test "Set many keys in the cluster" {
        for {set i 0} {$i < 5000} {incr i} {
            $cluster set $i $i
            assert { [$cluster get $i] eq $i }
        }
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
        assert_match "*MOVED*" $e_moved1
        # ASK REPLY
        catch {$nodefrom(link) set "abc{$key}" "newVal"} e_ask1
        assert_match "*ASK*" $e_ask1

        # UNSTABLE REPLY
        assert_error "*TRYAGAIN*" {$nodefrom(link) mset "a{$key}" "newVal" $key "newVal2"}

        # Connecting using another protocol
        array set nodefrom_pport [$cluster_pport masternode_for_slot $slot]
        array set nodeto_pport [$cluster_pport masternode_notfor_slot $slot]

        # MOVED REPLY
        catch {$nodeto_pport(link) set $key "newVal"} e_moved2
        assert_match "*MOVED*" $e_moved2
        # ASK REPLY
        catch {$nodefrom_pport(link) set "abc{$key}" "newVal"} e_ask2
        assert_match "*ASK*" $e_ask2
        # Compare MOVED error's port
        set port1 [get_port_from_moved_error $e_moved1]
        set port2 [get_port_from_moved_error $e_moved2]
        assert_not_equal $port1 $port2
        assert_equal $port1 $nodefrom(port)
        assert_equal $port2 [get_pport_by_port $nodefrom(port)]
        # Compare ASK error's port
        set port1 [get_port_from_moved_error $e_ask1]
        set port2 [get_port_from_moved_error $e_ask2]
        assert_not_equal $port1 $port2
        assert_equal $port1 $nodeto(port)
        assert_equal $port2 [get_pport_by_port $nodeto(port)]
    }

    config_set_all_nodes cluster-allow-replica-migration yes
}

if {$::tls} {

    source "../tests/includes/init-tests.tcl"

    test "Create a 3 nodes cluster" {
        cluster_create_with_continuous_slots 3 3
    }

    test "Cluster is up" {
        assert_cluster_state ok
    }

    # Test TLS service when "tls-cluster" is enabled
    tls_service_tests

    # Clean up all nodes and spawn new instances for TLS tests
    # in a cluster with "tls-cluster" is disbled
    cleanup
    set ::redis_instances {}
    spawn_instance redis $::redis_base_port 6 0 {
        "cluster-enabled yes"
        "appendonly yes"
        "enable-protected-configs yes"
        "enable-debug-command yes"
        "save ''"
    }
    join_nodes_in_cluster 1

    test "Create a 3 nodes cluster" {
        cluster_create_with_continuous_slots 3 3
    }

    test "Cluster is up" {
        assert_cluster_state ok
    }

    # Test TLS service when "tls-cluster" is disabled
    tls_service_tests
}