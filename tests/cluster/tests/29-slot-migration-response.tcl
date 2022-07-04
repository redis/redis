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
    assert_error "*MOVED*" {$nodeto(link) set $key "newVal"}

    # ASK REPLY
    assert_error "*ASK*" {$nodefrom(link) set "abc{$key}" "newVal"}

    # UNSTABLE REPLY
    assert_error "*TRYAGAIN*" {$nodefrom(link) mset "a{$key}" "newVal" $key "newVal2"}
}

config_set_all_nodes cluster-allow-replica-migration yes
