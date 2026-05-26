# Test that redis-cli --cluster rebalance doesn't fail with CROSSSLOT
# when --user is specified without -a (no password).

source "../tests/includes/init-tests.tcl"
source "../tests/includes/utils.tcl"

test "Create a 3 nodes cluster" {
    cluster_create_with_continuous_slots 3 3
}

test "Cluster is up" {
    assert_cluster_state ok
}

test "Set up ACL users for testing" {
    foreach_redis_id id {
        R $id ACL SETUSER testuser on nopass +@all
        R $id ACL SETUSER testuser2 on >testpass +@all
    }
}

proc write_keys_to_master0 {key_prefix} {
    set start_node_port [get_instance_attrib redis 0 port]
    set cluster [redis_cluster 127.0.0.1:$start_node_port]
    set slot_ranges [dict get [get_myself 0] slots]
    set count 0
    foreach range $slot_ranges {
        set start [lindex [split $range "-"] 0]
        set end [lindex [split $range "-"] 1]
        if {$end eq ""} {set end $start}
        for {set s $start} {$s <= $end} {incr s} {
            $cluster set "$key_prefix:$s:a" "value:$s"
            $cluster set "$key_prefix:$s:b" "value:$s"
            incr count 2
            if {$count >= 100} break
        }
        if {$count >= 100} break
    }
    $cluster close
}

test "Write keys to master 0 slots" {
    write_keys_to_master0 "key"
}

test "Rebalance without --user and without -a should succeed" {
    set master0_id [dict get [get_myself 0] id]
    catch {
        exec ../../../src/redis-cli --cluster rebalance \
            127.0.0.1:[get_instance_attrib redis 0 port] \
            {*}[rediscli_tls_config "../../../tests"] \
            --cluster-weight ${master0_id}=0 \
            --cluster-timeout 10000
    } output
    assert_no_match "*CROSSSLOT*" $output
    assert_no_match "*CROSS*SLOT*" $output
}

test "Re-distribute slots to master 0 for next test" {
    catch {
        exec ../../../src/redis-cli --cluster rebalance \
            127.0.0.1:[get_instance_attrib redis 0 port] \
            {*}[rediscli_tls_config "../../../tests"] \
            --cluster-timeout 10000
    } output
    wait_cluster_stable
}

test "Write more keys to master 0 slots" {
    write_keys_to_master0 "key2"
}

test "Rebalance with --user but no -a should not CROSSSLOT" {
    # This used to fail with CROSSSLOT because the empty string
    # argument in MIGRATE was treated as a key with slot 0
    set master0_id [dict get [get_myself 0] id]
    catch {
        exec ../../../src/redis-cli --cluster rebalance \
            127.0.0.1:[get_instance_attrib redis 0 port] \
            {*}[rediscli_tls_config "../../../tests"] \
            --user testuser \
            --cluster-weight ${master0_id}=0 \
            --cluster-timeout 10000
    } output
    assert_no_match "*CROSSSLOT*" $output
    assert_no_match "*CROSS*SLOT*" $output
}

test "Re-distribute slots to master 0 for next test" {
    catch {
        exec ../../../src/redis-cli --cluster rebalance \
            127.0.0.1:[get_instance_attrib redis 0 port] \
            {*}[rediscli_tls_config "../../../tests"] \
            --cluster-timeout 10000
    } output
    wait_cluster_stable
}

test "Write more keys to master 0 slots" {
    write_keys_to_master0 "key3"
}

test "Rebalance with --user and -a should succeed" {
    set master0_id [dict get [get_myself 0] id]
    catch {
        exec ../../../src/redis-cli --cluster rebalance \
            127.0.0.1:[get_instance_attrib redis 0 port] \
            {*}[rediscli_tls_config "../../../tests"] \
            --user testuser2 \
            -a testpass \
            --no-auth-warning \
            --cluster-weight ${master0_id}=0 \
            --cluster-timeout 10000
    } output
    assert_no_match "*CROSSSLOT*" $output
    assert_no_match "*CROSS*SLOT*" $output
}
