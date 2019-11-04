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

}
