source "../tests/includes/init-tests.tcl"

test "Create a 2 nodes cluster" {
    create_cluster 2 2
}

test "Cluster is up" {
    assert_cluster_state ok
}

test "Cluster is writable" {
    cluster_write_test 0
}

set cluster [redis_cluster 127.0.0.1:[get_instance_attrib redis 0 port]]

test "CLUSTER Unknown node" {
    catch {[$cluster CLUSTER replicas 12]} e
    if {![string match "*Unknown node*" $e]} {
        fail "Should contain Unknown node error in CLUSTER replicas"
    }

    catch {[$cluster CLUSTER replicate 12]} e
    if {![string match "*Unknown node*" $e]} {
        fail "Should contain Unknown node error in CLUSTER replicate"
    }

    catch {[$cluster CLUSTER forget 12]} e
    if {![string match "*Unknown node*" $e]} {
        fail "Should contain Unknown node error in CLUSTER forget"
    }
}

#CLUSTER REPLICAS

test "CLUSTER replicas node not master" {
    set slave_id [dict get [get_myself 2] id]
    catch {[$cluster CLUSTER replicas $slave_id]} e
    if {![string match "*The specified node is not a master*" $e]} {
        fail "Should contain not a master error"
    }
}

#CLUSTER REPLICATE

test "CLUSTER replicate node can only replicate master" {
    set slave_id [dict get [get_myself 2] id]
    catch {[$cluster CLUSTER replicate $slave_id]} e
    if {![string match "*I can only replicate a master, not a replica*" $e]} {
        fail "Should contain i can only replicate master error"
    }
}

test "CLUSTER replicate node must be empty" {
    set slave_id [dict get [get_myself 8] id]
    catch {[R 0 CLUSTER replicate $slave_id]} e
    if {![string match "*node must be empty and without assigned slots*" $e]} {
        fail "Should contain node must be empty and without assigned slots error"
    }
    
}

test "CLUSTER replicate self" {
    set slave_id [dict get [get_myself 3] id]
    catch {[R 3 CLUSTER replicate $slave_id]} e
    if {![string match "*Can't replicate myself*" $e]} {
        fail "Should contain can't replicate myself error"
    }
}

# CLUSTR FORGET

test "CLUSTER forget self" {
    set slave_id [dict get [get_myself 0] id]
    catch {[R 0 CLUSTER forget $slave_id]} e
    if {![string match "*can't forget myself*" $e]} {
        fail "Should contain can't forget myself error"
    }
}

test "CLUSTER forget master" {
    set master_id [dict get [get_myself 0] id]
    catch {[R 2 CLUSTER forget $master_id]} e
    if {![string match "*Can't forget my master*" $e]} {
        fail "Should contain Can't forget my master error"
    }
}
