set testmodule [file normalize tests/modules/replicationcompat.so]
foreach ::compatibility_test_configuration {shared local-port} {
    start_cluster 2 0 [list tags {external:skip cluster modules} overrides [list loadmodule "$testmodule $::compatibility_test_configuration"]] {
        test "ASM validates module compatibility ($::compatibility_test_configuration)" {
            R 0 set {{06S}:original} value
            set task [R 1 cluster migration import 0 0]
            wait_for_condition 200 50 {
                [dict get [lindex [R 1 cluster migration status id $task] 0] state] in {completed failed}
            } else { fail "Migration did not finish" }
            set status [lindex [R 1 cluster migration status id $task] 0]
            if {$::compatibility_test_configuration eq "shared"} {
                assert_equal completed [dict get $status state]
                assert_equal value [R 1 get {{06S}:original}]
            } else {
                assert_equal failed [dict get $status state]
                assert_match {*MODULECONFIG*} [dict get $status last_error]
                assert_equal value [R 0 get {{06S}:original}]
                assert_equal 0 [R 1 cluster countkeysinslot 0]
            }
        }
    }
}
