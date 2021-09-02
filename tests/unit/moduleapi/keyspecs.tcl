set testmodule [file normalize tests/modules/keyspecs.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test "Module key specs: Legacy" {
        set reply [r command info kspec.legacy]
        assert_equal $reply {{kspec.legacy -1 {} 1 2 1 {} {{flags read begin_search {type index spec {index 1}} find_keys {type range spec {lastkey 0 keystep 1 limit 0}}} {flags write begin_search {type index spec {index 2}} find_keys {type range spec {lastkey 0 keystep 1 limit 0}}}}}}
    }

    test "Module key specs: Complex specs, case 1" {
        set reply [r command info kspec.complex1]
        assert_equal $reply {{kspec.complex1 -1 movablekeys 1 1 1 {} {{flags {} begin_search {type index spec {index 1}} find_keys {type range spec {lastkey 0 keystep 1 limit 0}}} {flags write begin_search {type keyword spec {keyword STORE startfrom 2}} find_keys {type range spec {lastkey 0 keystep 1 limit 0}}} {flags read begin_search {type keyword spec {keyword KEYS startfrom 2}} find_keys {type keynum spec {keynumidx 0 firstkey 1 keystep 1}}}}}}
    }

    test "Module key specs: Complex specs, case 2" {
        set reply [r command info kspec.complex2]
        assert_equal $reply {{kspec.complex2 -1 movablekeys 1 2 1 {} {{flags write begin_search {type keyword spec {keyword STORE startfrom 5}} find_keys {type range spec {lastkey 0 keystep 1 limit 0}}} {flags read begin_search {type index spec {index 1}} find_keys {type range spec {lastkey 0 keystep 1 limit 0}}} {flags read begin_search {type index spec {index 2}} find_keys {type range spec {lastkey 0 keystep 1 limit 0}}} {flags write begin_search {type index spec {index 3}} find_keys {type keynum spec {keynumidx 0 firstkey 1 keystep 1}}} {flags write begin_search {type keyword spec {keyword MOREKEYS startfrom 5}} find_keys {type range spec {lastkey -1 keystep 1 limit 0}}}}}}
    }
}
