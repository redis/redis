set testmodule [file normalize tests/modules/keyspecs.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test "Module key specs: Legacy" {
        set reply [r command info kspec.legacy]
        assert_equal $reply {{kspec.legacy -1 {} 1 2 1 {} {{read {range {1 1 1}}} {write {range {2 2 1}}}}}}
    }

    test "Module key specs: Complex specs, case 1" {
        set reply [r command info kspec.complex1]
        assert_equal $reply {{kspec.complex1 -1 movablekeys 1 1 1 {} {{read {range {1 1 1}}} {read {keyword {KEYS 1 1}}}}}}
    }

    test "Module key specs: Complex specs, case 2" {
        set reply [r command info kspec.complex2]
        assert_equal $reply {{kspec.complex2 -1 movablekeys 1 2 1 {} {{read {keyword {KEYS 1 1}}} {read {range {1 1 1}}} {write {range {2 2 1}}} {write {keynum {3 4 1}}} {read {keyword {MOREKEYS 1 1}}}}}}
    }
}
