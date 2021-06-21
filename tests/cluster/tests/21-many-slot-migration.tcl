# Tests for many simlutaneous migrations.

# TODO: Test is currently disabled until it is stabilized (fixing the test
# itself or real issues in Redis).

if {false} {

source "../tests/includes/init-tests.tcl"
source "../tests/includes/utils.tcl"

# TODO: This test currently runs without replicas, as failovers (which may
# happen on lower-end CI platforms) are still not handled properly by the
# cluster during slot migration (related to #6339).

test "Create a 10 nodes cluster" {
    create_cluster 10 0
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
    for {set i 0} {$i < 40000} {incr i} {
        $cluster set key:$i val:$i
    }
}

test "Keys are accessible" {
    for {set i 0} {$i < 40000} {incr i} {
        assert { [$cluster get key:$i] eq "val:$i" }
    }
}

test "Init migration of many slots" {
    for {set slot 0} {$slot < 1000} {incr slot} {
        array set nodefrom [$cluster masternode_for_slot $slot]
        array set nodeto [$cluster masternode_notfor_slot $slot]

        $nodefrom(link) cluster setslot $slot migrating $nodeto(id)
        $nodeto(link) cluster setslot $slot importing $nodefrom(id)
    }
}

test "Fix cluster" {
    wait_for_cluster_propagation
    fix_cluster $nodefrom(addr)
}

test "Keys are accessible" {
    for {set i 0} {$i < 40000} {incr i} {
        assert { [$cluster get key:$i] eq "val:$i" }
    }
}

config_set_all_nodes cluster-allow-replica-migration yes
}
