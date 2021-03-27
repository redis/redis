# Tests for fixing migrating slot at all stages:
# 1. when migration is half inited on "migrating" node
# 2. when migration is half inited on "importing" node
# 3. migration inited, but not finished
# 4. migration is half finished on "migrating" node
# 5. migration is half finished on "importing" node
source "../tests/includes/init-tests.tcl"
source "../../../tests/support/cli.tcl"

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

reset_cluster

test "Set key" {
    # 'aga' key is in 609 slot
    $cluster set aga xyz
}

test "Key is accessible" {
    assert {[$cluster get aga] eq "xyz"}
}

test "Fix doesn't need to fix anything" {
    set code [catch {exec ../../../src/redis-cli {*}[rediscli_tls_config "../../../tests"] --cluster fix $nodefrom(addr) << yes} result]
    assert {$code == 0}
}

test "Key is accessible" {
    assert {[$cluster get aga] eq "xyz"}
}

test "Half init migration in 'migrating'" {
    $nodefrom(link) cluster setslot 609 migrating $nodeto(id)
}

test "Fix cluster" {
    fix_cluster $nodefrom(addr)
}

test "Key is accessible" {
    assert {[$cluster get aga] eq "xyz"}
}

test "Half init migration in 'importing'" {
    $nodeto(link) cluster setslot 609 importing $nodefrom(id)
}

test "Fix cluster" {
    fix_cluster $nodefrom(addr)
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
    fix_cluster $nodefrom(addr)
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
    fix_cluster $nodefrom(addr)
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
    fix_cluster $nodefrom(addr)
}

test "Key is accessible" {
    assert {[$cluster get aga] eq "xyz"}
}
