source "../tests/includes/init-tests.tcl"

test "Create a 10 nodes cluster" {
    create_cluster 10 10
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
    }
}

proc get_nodes {slot} {
    uplevel 1 {
        array set nodefrom [$cluster masternode_for_slot $slot]
        array set nodeto [$cluster masternode_notfor_slot $slot]
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
        get_nodes $slot
        $nodefrom(link) cluster setslot $slot migrating $nodeto(id)
        $nodeto(link) cluster setslot $slot importing $nodefrom(id)
    }
}

test "Fix cluster" {
    fix_cluster
}

test "Keys are accessible" {
    for {set i 0} {$i < 40000} {incr i} {
        assert { [$cluster get key:$i] eq "val:$i" }
    }
}

