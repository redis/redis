set testmodule [file normalize tests/modules/keyspecs.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test "Module key specs: Legacy" {
        set reply [lindex [r command info kspec.legacy] 0]
        # Verify (first, last, step)
        assert_equal [lindex $reply 3] 1
        assert_equal [lindex $reply 4] 2
        assert_equal [lindex $reply 5] 1
        # Verify key-specs
        set keyspecs [lindex $reply 8]
        assert_equal [lindex $keyspecs 0] {flags read begin_search {type index spec {index 1}} find_keys {type range spec {lastkey 0 keystep 1 limit 0}}}
        assert_equal [lindex $keyspecs 1] {flags write begin_search {type index spec {index 2}} find_keys {type range spec {lastkey 0 keystep 1 limit 0}}}
    }

    test "Module key specs: Complex specs, case 1" {
        set reply [lindex [r command info kspec.complex1] 0]
        # Verify (first, last, step)
        assert_equal [lindex $reply 3] 1
        assert_equal [lindex $reply 4] 1
        assert_equal [lindex $reply 5] 1
        # Verify key-specs
        set keyspecs [lindex $reply 8]
        assert_equal [lindex $keyspecs 0] {flags {} begin_search {type index spec {index 1}} find_keys {type range spec {lastkey 0 keystep 1 limit 0}}}
        assert_equal [lindex $keyspecs 1] {flags write begin_search {type keyword spec {keyword STORE startfrom 2}} find_keys {type range spec {lastkey 0 keystep 1 limit 0}}}
        assert_equal [lindex $keyspecs 2] {flags read begin_search {type keyword spec {keyword KEYS startfrom 2}} find_keys {type keynum spec {keynumidx 0 firstkey 1 keystep 1}}}
    }

    test "Module key specs: Complex specs, case 2" {
        set reply [lindex [r command info kspec.complex2] 0]
        # Verify (first, last, step)
        assert_equal [lindex $reply 3] 1
        assert_equal [lindex $reply 4] 2
        assert_equal [lindex $reply 5] 1
        # Verify key-specs
        set keyspecs [lindex $reply 8]
        assert_equal [lindex $keyspecs 0] {flags write begin_search {type keyword spec {keyword STORE startfrom 5}} find_keys {type range spec {lastkey 0 keystep 1 limit 0}}}
        assert_equal [lindex $keyspecs 1] {flags read begin_search {type index spec {index 1}} find_keys {type range spec {lastkey 0 keystep 1 limit 0}}}
        assert_equal [lindex $keyspecs 2] {flags read begin_search {type index spec {index 2}} find_keys {type range spec {lastkey 0 keystep 1 limit 0}}}
        assert_equal [lindex $keyspecs 3] {flags write begin_search {type index spec {index 3}} find_keys {type keynum spec {keynumidx 0 firstkey 1 keystep 1}}}
        assert_equal [lindex $keyspecs 4] {flags write begin_search {type keyword spec {keyword MOREKEYS startfrom 5}} find_keys {type range spec {lastkey -1 keystep 1 limit 0}}}
    }

    test "Module command list filtering" {
        ;# Note: we piggyback this tcl file to test the general functionality of command list filtering
        set reply [r command list filterby module keyspecs]
        assert_equal [lsort $reply] {kspec.complex1 kspec.complex2 kspec.legacy}
    }
}
