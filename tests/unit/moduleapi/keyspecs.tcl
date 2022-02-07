set testmodule [file normalize tests/modules/keyspecs.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test "Module key specs: No spec, only legacy triple" {
        set reply [lindex [r command info kspec.none] 0]
        # Verify (first, last, step) and not movablekeys
        assert_equal [lindex $reply 2] {module}
        assert_equal [lindex $reply 3] 1
        assert_equal [lindex $reply 4] 3
        assert_equal [lindex $reply 5] 2
        # Verify key-spec auto-generated from the legacy triple
        set keyspecs [lindex $reply 8]
        assert_equal [llength $keyspecs] 1
        assert_equal [lindex $keyspecs 0] {flags {} begin_search {type index spec {index 1}} find_keys {type range spec {lastkey 2 keystep 2 limit 0}}}
    }

    test "Module key specs: Two ranges" {
        set reply [lindex [r command info kspec.tworanges] 0]
        # Verify (first, last, step) and not movablekeys
        assert_equal [lindex $reply 2] {module}
        assert_equal [lindex $reply 3] 1
        assert_equal [lindex $reply 4] 2
        assert_equal [lindex $reply 5] 1
        # Verify key-specs
        set keyspecs [lindex $reply 8]
        assert_equal [lindex $keyspecs 0] {flags {RO access} begin_search {type index spec {index 1}} find_keys {type range spec {lastkey 0 keystep 1 limit 0}}}
        assert_equal [lindex $keyspecs 1] {flags {RW update} begin_search {type index spec {index 2}} find_keys {type range spec {lastkey 0 keystep 1 limit 0}}}
    }

    test "Module key specs: Keyword-only spec clears the legacy triple" {
        set reply [lindex [r command info kspec.keyword] 0]
        # Verify (first, last, step) and movablekeys
        assert_equal [lindex $reply 2] {module movablekeys}
        assert_equal [lindex $reply 3] 0
        assert_equal [lindex $reply 4] 0
        assert_equal [lindex $reply 5] 0
        # Verify key-specs
        set keyspecs [lindex $reply 8]
        assert_equal [lindex $keyspecs 0] {flags {RO access} begin_search {type keyword spec {keyword KEYS startfrom 1}} find_keys {type range spec {lastkey -1 keystep 1 limit 0}}}
    }

    test "Module key specs: Complex specs, case 1" {
        set reply [lindex [r command info kspec.complex1] 0]
        # Verify (first, last, step) and movablekeys
        assert_equal [lindex $reply 2] {module movablekeys}
        assert_equal [lindex $reply 3] 1
        assert_equal [lindex $reply 4] 1
        assert_equal [lindex $reply 5] 1
        # Verify key-specs
        set keyspecs [lindex $reply 8]
        assert_equal [lindex $keyspecs 0] {flags RO begin_search {type index spec {index 1}} find_keys {type range spec {lastkey 0 keystep 1 limit 0}}}
        assert_equal [lindex $keyspecs 1] {flags {RW update} begin_search {type keyword spec {keyword STORE startfrom 2}} find_keys {type range spec {lastkey 0 keystep 1 limit 0}}}
        assert_equal [lindex $keyspecs 2] {flags {RO access} begin_search {type keyword spec {keyword KEYS startfrom 2}} find_keys {type keynum spec {keynumidx 0 firstkey 1 keystep 1}}}
    }

    test "Module key specs: Complex specs, case 2" {
        set reply [lindex [r command info kspec.complex2] 0]
        # Verify (first, last, step) and movablekeys
        assert_equal [lindex $reply 2] {module movablekeys}
        assert_equal [lindex $reply 3] 1
        assert_equal [lindex $reply 4] 2
        assert_equal [lindex $reply 5] 1
        # Verify key-specs
        set keyspecs [lindex $reply 8]
        assert_equal [lindex $keyspecs 0] {flags {RW update} begin_search {type keyword spec {keyword STORE startfrom 5}} find_keys {type range spec {lastkey 0 keystep 1 limit 0}}}
        assert_equal [lindex $keyspecs 1] {flags {RO access} begin_search {type index spec {index 1}} find_keys {type range spec {lastkey 0 keystep 1 limit 0}}}
        assert_equal [lindex $keyspecs 2] {flags {RO access} begin_search {type index spec {index 2}} find_keys {type range spec {lastkey 0 keystep 1 limit 0}}}
        assert_equal [lindex $keyspecs 3] {flags {RW update} begin_search {type index spec {index 3}} find_keys {type keynum spec {keynumidx 0 firstkey 1 keystep 1}}}
        assert_equal [lindex $keyspecs 4] {flags {RW update} begin_search {type keyword spec {keyword MOREKEYS startfrom 5}} find_keys {type range spec {lastkey -1 keystep 1 limit 0}}}
    }

    test "Module command list filtering" {
        ;# Note: we piggyback this tcl file to test the general functionality of command list filtering
        set reply [r command list filterby module keyspecs]
        assert_equal [lsort $reply] {kspec.complex1 kspec.complex2 kspec.keyword kspec.none kspec.tworanges}
    }

    test "Unload the module - keyspecs" {
        assert_equal {OK} [r module unload keyspecs]
    }
}
