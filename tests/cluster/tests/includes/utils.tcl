source "../../../tests/support/cli.tcl"

proc config_set_all_nodes {keyword value} {
    foreach_redis_id id {
        R $id config set $keyword $value
    }
}

proc fix_cluster {addr} {
    set code [catch {
        exec ../../../src/redis-cli {*}[rediscli_tls_config "../../../tests"] --cluster fix $addr << yes
    } result]
    if {$code != 0} {
        puts "redis-cli --cluster fix returns non-zero exit code, output below:\n$result"
    }
    # Note: redis-cli --cluster fix may return a non-zero exit code if nodes don't agree,
    # but we can ignore that and rely on the check below.
    assert_cluster_state ok
    wait_for_condition 100 100 {
        [catch {exec ../../../src/redis-cli {*}[rediscli_tls_config "../../../tests"] --cluster check $addr} result] == 0
    } else {
        puts "redis-cli --cluster check returns non-zero exit code, output below:\n$result"
        fail "Cluster could not settle with configuration"
    }
}
