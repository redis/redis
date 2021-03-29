source "../../../tests/support/cli.tcl"

proc fix_cluster {addr} {
    set code [catch {
        exec ../../../src/redis-cli {*}[rediscli_tls_config "../../../tests"] --cluster fix $addr << yes
    } result]
    if {$code != 0 && $::verbose} {
        puts $result
    }
    assert {$code == 0}
    assert_cluster_state ok
    wait_for_condition 1000 10 {
        [catch {exec ../../../src/redis-cli {*}[rediscli_tls_config "../../../tests"] --cluster check $addr} _] == 0
    } else {
        fail "Cluster could not settle with configuration"
    }
}
