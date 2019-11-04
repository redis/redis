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

    test {test module db commands} {
        r set x foo
        set key [r test.randomkey]
        assert_equal $key "x"
        assert_equal [r test.dbsize] 1
        r test.flushall
        assert_equal [r test.dbsize] 0
    }

    test {test modle lru api} {
        r set x foo
        set lru [r test.getlru x]
        assert { $lru <= 1 }
        r test.setlru x 100
        set idle [r object idletime x]
        assert { $idle >= 100 }
        set lru [r test.getlru x]
        assert { $lru >= 100 }
    }
}
