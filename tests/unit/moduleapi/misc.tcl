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

    test {test RM_Call recursive} {
        set info [r test.call_generic test.call_generic info commandstats]
        assert { [string match "*cmdstat_module*" $info] }
    }

    test {test redis version} {
        set version [s redis_version]
        assert_equal $version [r test.redisversion]
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

    test {test module keyexists} {
        r set x foo
        assert_equal 1 [r test.keyexists x]
        r del x
        assert_equal 0 [r test.keyexists x]
    }

    test {test module lru api} {
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

    test {test module lfu api} {
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

    test {test module clientinfo api} {
        # Test basic sanity and SSL flag
        set info [r test.clientinfo]
        set ssl_flag [expr $::tls ? {"ssl:"} : {":"}]

        assert { [dict get $info db] == 9 }
        assert { [dict get $info flags] == "${ssl_flag}::::" }

        # Test MULTI flag
        r multi
        r test.clientinfo
        set info [lindex [r exec] 0]
        assert { [dict get $info flags] == "${ssl_flag}::::multi" }

        # Test TRACKING flag
        r client tracking on
        set info [r test.clientinfo]
        assert { [dict get $info flags] == "${ssl_flag}::tracking::" }
    }

    test {test module getclientcert api} {
        set cert [r test.getclientcert]

        if {$::tls} {
            assert {$cert != ""}
        } else {
            assert {$cert == ""}
        }
    }

    test {test detached thread safe cnotext} {
        r test.log_tsctx "info" "Test message"
        verify_log_message 0 "*<misc> Test message*" 0
    }

    test {test RM_Call CLIENT INFO} {
        assert_match "*fd=-1*" [r test.call_generic client info]
    }

    test {Unsafe command names are sanitized in INFO output} {
        r test.weird:cmd
        set info [r info commandstats]
        assert_match {*cmdstat_test.weird_cmd:calls=1*} $info
    }

    test {test monotonic time} {
        set x [r test.monotonic_time]
        assert { [r test.monotonic_time] >= $x }
    }

    test "Unload the module - misc" {
        assert_equal {OK} [r module unload misc]
    }
}
