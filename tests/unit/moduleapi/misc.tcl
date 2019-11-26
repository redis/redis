set testmodule [file normalize tests/modules/misc.so]


start_server {tags {"modules"}} {
    r module load $testmodule

    test {test RM_Call} {
        set info [r test.call_info commandstats]
        # cmdstat is not in a default section, so we also test an argument was passed
        assert { [string match "*cmdstat_module*" $info] }
    }

    test {test RM_Call args array} {
        set info [r test.call_generic info commandstats]
        # cmdstat is not in a default section, so we also test an argument was passed
        assert { [string match "*cmdstat_module*" $info] }
    }

    test {test long double conversions} {
        set ld [r test.ld_conversion]
        assert {[string match $ld "0.00000000000000001"]}
    }

    test {test module db commands} {
        r set x foo
        set key [r test.randomkey]
        assert_equal $key "x"
        assert_equal [r test.dbsize] 1
        r test.flushall
        assert_equal [r test.dbsize] 0
    }

    test {test modle lru api} {
        r config set maxmemory-policy allkeys-lru
        r set x foo
        set lru [r test.getlru x]
        assert { $lru <= 1000 }
        set was_set [r test.setlru x 100000]
        assert { $was_set == 1 }
        set idle [r object idletime x]
        assert { $idle >= 100 }
        set lru [r test.getlru x]
        assert { $lru >= 100000 }
        r config set maxmemory-policy allkeys-lfu
        set lru [r test.getlru x]
        assert { $lru == -1 }
        set was_set [r test.setlru x 100000]
        assert { $was_set == 0 }
    }
    r config set maxmemory-policy allkeys-lru

    test {test modle lfu api} {
        r config set maxmemory-policy allkeys-lfu
        r set x foo
        set lfu [r test.getlfu x]
        assert { $lfu >= 1 }
        set was_set [r test.setlfu x 100]
        assert { $was_set == 1 }
        set freq [r object freq x]
        assert { $freq <= 100 }
        set lfu [r test.getlfu x]
        assert { $lfu <= 100 }
        r config set maxmemory-policy allkeys-lru
        set lfu [r test.getlfu x]
        assert { $lfu == -1 }
        set was_set [r test.setlfu x 100]
        assert { $was_set == 0 }
    }

}
