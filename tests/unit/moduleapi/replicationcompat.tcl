set testmodule [file normalize tests/modules/replicationcompat.so]

tags "modules external:skip" {
    foreach rdbchannel {yes no} {
        test "Matching module configurations replicate (RDB channel $rdbchannel)" {
            start_server [list overrides [list loadmodule "$testmodule shared" repl-rdb-channel $rdbchannel]] {
                set replica [srv 0 client]
                start_server [list overrides [list loadmodule "$testmodule shared" repl-rdb-channel $rdbchannel repl-diskless-sync-delay 0]] {
                    r set before initial
                    $replica replicaof [srv 0 host] [srv 0 port]
                    wait_for_sync $replica
                    assert_equal initial [$replica get before]
                    r set after streamed
                    wait_for_condition 100 50 {[$replica get after] eq "streamed"} else {
                        fail "Incremental replication failed"
                    }
                    r client kill type replica
                    wait_for_sync $replica
                    r set reconnected yes
                    wait_for_condition 100 50 {[$replica get reconnected] eq "yes"} else {
                        fail "Replication failed after reconnect"
                    }
                }
            }
        }
    }
    foreach pair {{a b} {a none} {none a}} {
        lassign $pair source_config destination_config
        test "Incompatible module configurations preserve destination ($pair)" {
            start_server [list overrides [list loadmodule "$testmodule $destination_config"]] {
                set replica [srv 0 client]
                $replica set preserve original
                start_server [list overrides [list loadmodule "$testmodule $source_config" repl-diskless-sync-delay 0]] {
                    r set source data
                    $replica replicaof [srv 0 host] [srv 0 port]
                    # Observe a completed rejection, not just a not-yet-established link.
                    wait_for_log_messages -1 {"*Module replication compatibility check failed*"} 0 100 50
                    assert_equal down [s -1 master_link_status]
                    assert_equal original [$replica get preserve]
                    assert_equal 0 [$replica exists source]
                    assert_equal 0 [s 0 sync_full]
                }
            }
        }
    }
    start_server {} {
        test "Replication compatibility cannot be registered by runtime MODULE LOAD" {
            assert_error {*Error loading the extension*} {r module load $testmodule shared}
        }
    }
    start_server [list overrides [list loadmodule "$testmodule shared"]] {
        test "Replication compatibility cannot be unloaded" {
            assert_error {*immutable until restart*} {r module unload replicationcompat}
        }
        test "Missing compatibility preflight rejects direct SYNC and PSYNC" {
            assert_error {*MODULECONFIG*} {r sync}
            assert_error {*MODULECONFIG*} {r psync ? -1}
        }
        test "Mismatched compatibility preflight is explicit" {
            assert_error {*MODULECONFIG*} {r replconf module-compatibility incorrect}
        }
    }
}

tags "modules external:skip" {
    test "Module compatibility is independent of module load order" {
        start_server [list config_lines [list loadmodule "$testmodule first moduleA" loadmodule "$testmodule second moduleB"]] {
            set expected [s module_replication_compatibility]
            assert_equal 64 [string length $expected]
            start_server [list config_lines [list loadmodule "$testmodule second moduleB" loadmodule "$testmodule first moduleA"]] {
                assert_equal $expected [s module_replication_compatibility]
            }
        }
    }
}
