# Test fix of half migrated slot.
# If old slot owner already received 'setslot <slotid> node <newowner>' command,
# but new owner didn't, then where is now "owner" and it is still 'importing' in new owner.
# Since new owner alread has slots, it fails to accept "CLUSTER ADDSLOTS" command.

source "../tests/includes/init-tests.tcl"

test "Create a 2 nodes cluster" {
    create_cluster 2 0
}

test "Cluster is up" {
    assert_cluster_state ok
}

set cluster [redis_cluster 127.0.0.1:[get_instance_attrib redis 0 port]]
catch {unset nodefrom}
catch {unset nodeto}
proc reset_cluster {} {
    uplevel 1 {
        $cluster refresh_nodes_map
        array set nodefrom [$cluster masternode_for_slot 609]
        array set nodeto [$cluster masternode_notfor_slot 609]
    }
}

proc fix_cluster {} {
    uplevel 1 {
        set code [catch {
            exec ../../../src/redis-cli --cluster fix $nodefrom(addr) << yes
        } result]
        if {$code != 0 && $::verbose} {
            puts $result
        }
        assert {$code == 0}
        assert_cluster_state ok
        wait_for_condition 1000 10 {
            [catch {exec ../../../src/redis-cli --cluster check $nodefrom(addr)} _] == 0
        } else {
            fail "Cluster could not settle with configuration"
        }
    }
}

reset_cluster

test "Set key" {
    # 'aga' key is in 609 slot
    $cluster set aga xyz
}

test "Key is accessible" {
    assert {[$cluster get aga] eq "xyz"}
}

test "Fix doesn't need to fix anything" {
    set code [catch {exec ../../../src/redis-cli --cluster fix $nodefrom(addr) << yes} result]
    assert {$code == 0}
}

test "Key is accessible" {
    assert {[$cluster get aga] eq "xyz"}
}

test "Half init migration in 'migrating'" {
    $nodefrom(link) cluster setslot 609 migrating $nodeto(id)
}

test "Fix cluster" {
    fix_cluster
}

test "Key is accessible" {
    assert {[$cluster get aga] eq "xyz"}
}

test "Half init migration in 'importing'" {
    $nodeto(link) cluster setslot 609 importing $nodefrom(id)
}

test "Fix cluster" {
    fix_cluster
}

test "Key is accessible" {
    assert {[$cluster get aga] eq "xyz"}
}

test "Init migration and move key" {
    $nodefrom(link) cluster setslot 609 migrating $nodeto(id)
    $nodeto(link) cluster setslot 609 importing $nodefrom(id)
    $nodefrom(link) migrate $nodeto(host) $nodeto(port) aga 0 10000
}

test "Key is accessible" {
    assert {[$cluster get aga] eq "xyz"}
}

test "Fix cluster" {
    fix_cluster
}

test "Key is accessible" {
    assert {[$cluster get aga] eq "xyz"}
}

reset_cluster

test "Move key again" {
    $nodefrom(link) cluster setslot 609 migrating $nodeto(id)
    $nodeto(link) cluster setslot 609 importing $nodefrom(id)
    $nodefrom(link) migrate $nodeto(host) $nodeto(port) aga 0 10000
}

test "Key is accessible" {
    assert {[$cluster get aga] eq "xyz"}
}

test "Half-finish migration" {
    # half finish migration on 'migrating' node
    $nodefrom(link) cluster setslot 609 node $nodeto(id)
}

test "Fix cluster" {
    fix_cluster
}

reset_cluster

test "Move key back" {
    # 'aga' key is in 609 slot
    $nodefrom(link) cluster setslot 609 migrating $nodeto(id)
    $nodeto(link) cluster setslot 609 importing $nodefrom(id)
    $nodefrom(link) migrate $nodeto(host) $nodeto(port) aga 0 10000
}

test "Key is accessible" {
    assert {[$cluster get aga] eq "xyz"}
}

test "Half-finish migration" {
    # Now we half finish 'importing' node
    $nodeto(link) cluster setslot 609 node $nodeto(id)
}

test "Fix cluster again" {
    fix_cluster
}

test "Key is accessible" {
    assert {[$cluster get aga] eq "xyz"}
}
