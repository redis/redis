# Check the basic monitoring and failover capabilities.

source "../tests/includes/init-tests.tcl"

if {$::simulate_error} {
    test "This test will fail" {
        fail "Simulated error"
    }
}

test "Cluster nodes are reachable." {
    foreach_redis_id id {
        # Every node should just know itself.
        assert {[R $id ping] eq {PONG}}
    }
}
