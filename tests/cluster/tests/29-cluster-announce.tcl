source "../tests/includes/init-tests.tcl"

test "Create a 2 nodes cluster" {
    cluster_create_with_continuous_slots 2 2
}

test "Cluster should start ok" {
    assert_cluster_state ok
}

set master1 [Rn 0]
set master2 [Rn 1]


test "Test change cluster-announce-port and cluster-announce-tls-port at runtime" {
    set baseport [lindex [$master1 config get port] 1]
    set count [expr  [llength $::redis_instances] +1 ]
    set used_port [find_available_port $baseport $count]

    if {$::tls} {
        # Verify config set cluster-announce-tls-port
        $master1 config set cluster-announce-tls-port $used_port
        assert_match "*:$used_port@*" [$master1 CLUSTER NODES]
        wait_for_condition 50 100 {
            [string match "*:$used_port@*" [$master2 CLUSTER NODES]]
        } else {
            fail "NodeInfo(cluster-announce-tls-port) is not propagated via gossip"
        }

        # Verify restore default cluster-announce-tls-port
        $master1 config set cluster-announce-tls-port 0
        assert_match "*:$baseport@*" [$master1 CLUSTER NODES] 
    } else {
        # Verify config set cluster-announce-port
        $master1 config set cluster-announce-port $used_port
        assert_match "*:$used_port@*" [$master1 CLUSTER NODES]

        wait_for_condition 50 100 {
            [string match "*:$used_port@*" [$master2 CLUSTER NODES]]
        } else {
            fail "NodeInfo(cluster-announce-port) is not propagated via gossip"
        }

        # Verify restore default cluster-announce-port
        $master1 config set cluster-announce-port 0
        assert_match "*:$baseport@*" [$master1 CLUSTER NODES]    
    }
}


test "Test change cluster-announce-bus-port at runtime" {
    set baseport [lindex [$master1 config get port] 1]
    set count [expr  [llength $::redis_instances] +1 ]
    set used_port [find_available_port $baseport $count]

    # Verify config set cluster-announce-bus-port
    $master1 config set cluster-announce-bus-port $used_port
    assert_match "*@$used_port *" [$master1 CLUSTER NODES]
    wait_for_condition 50 100 {
            [string match "*@$used_port *" [$master2 CLUSTER NODES]]
        } else {
            fail "NodeInfo(cluster-announce-bus-port) is not propagated via gossip"
        }

    # Verify restore default cluster-announce-port
    set base_bus_port [expr $baseport + 10000 ]
    $master1 config set cluster-announce-bus-port 0
    assert_match "*@$base_bus_port *" [$master1 CLUSTER NODES]
}


