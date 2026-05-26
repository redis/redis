# Test that redis-cli --cluster rebalance doesn't fail with CROSSSLOT
# when --user is specified without -a (no password).
# This was a bug where argc was incorrectly incremented for the user
# parameter without auth, causing an empty string argument in MIGRATE.

source "../tests/includes/init-tests.tcl"
source "../tests/includes/utils.tcl"

test "Create a 3 nodes cluster" {
    cluster_create_with_continuous_slots 3 3
}

test "Cluster is up" {
    assert_cluster_state ok
}

test "Set up an ACL user for testing" {
    foreach_redis_id id {
        R $id ACL SETUSER testuser on nopass +@all
    }
}

test "Create keys using cluster client" {
    set start_node_port [get_instance_attrib redis 0 port]
    set cluster [redis_cluster 127.0.0.1:$start_node_port]
    $cluster set testkey1 v1
    $cluster set testkey2 v2
    $cluster set testkey3 v3
}

test "Rebalance with --user but no -a should not CROSSSLOT" {
    set master0_id [dict get [get_myself 0] id]
    catch {
        exec ../../../src/redis-cli \
            {*}[rediscli_tls_config "../../../tests"] \
            --user testuser \
            --cluster rebalance \
            127.0.0.1:[get_instance_attrib redis 0 port] \
            --cluster-weight ${master0_id}=0 \
            --cluster-timeout 10000
    } output
    assert_no_match "*CROSSSLOT*" $output
    assert_no_match "*CROSS*SLOT*" $output
}
