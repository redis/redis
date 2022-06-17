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

test "Set many keys" {
    for {set i 0} {$i < 5000} {incr i} {
        $cluster set $i $i
    }
}

test "Keys are accessible" {
    for {set i 0} {$i < 5000} {incr i} {
        assert { [$cluster get $i] eq $i }
    }
}

test "Migration of slot x" {

    set slot 10
    array set nodefrom [$cluster masternode_for_slot $slot]
    array set nodeto [$cluster masternode_notfor_slot $slot]

    $nodeto(link) cluster setslot $slot importing $nodefrom(id)
    $nodefrom(link) cluster setslot $slot migrating $nodeto(id)

    # Get a key from that slot
    set key [$nodefrom(link) cluster GETKEYSINSLOT $slot "1"]

    # MOVED REPLY
    catch {[$nodeto(link) set $key "newVal"]} f
    if {![string match "*MOVED*" $f]} {
        fail "Should contain MOVED when migrating"
    }

    # ASK REPLY
    catch {[$nodefrom(link) set "abc{$key}" "newVal"]} g
    if {![string match "*ASK*" $g]} {
        fail "Should contain ASK when migrating"
    }

    # UNSTABLE REPLY
    catch {[$nodefrom(link) mset "a{$key}" "newVal" $key "newVal2"]} f
    if {![string match "*TRYAGAIN*" $f]} {
        fail "It is in UNSTABLE status, client should see TRYAGAIN  error message when migrating"
    }
}
