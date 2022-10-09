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
    
    test {test unsigned long long conversions} {
        set ret [r test.ull_conversion]
        assert {[string match $ret "ok"]}
    }

    test {test module db commands} {
        r set x foo
        set key [r test.randomkey]
        assert_equal $key "x"
        assert_equal [r test.dbsize] 1
        r test.flushall
        assert_equal [r test.dbsize] 0
    }

    test {test RedisModule_ResetDataset do not reset functions} {
        r function load {#!lua name=lib
            redis.register_function('test', function() return 1 end)
        }
        assert_equal [r function list] {{library_name lib engine LUA functions {{name test description {} flags {}}}}}
        r test.flushall
        assert_equal [r function list] {{library_name lib engine LUA functions {{name test description {} flags {}}}}}
        r function flush
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

    test {test module get/set client name by id api} {
        catch { r test.getname } e
        assert_equal "-ERR No name" $e
        r client setname nobody
        catch { r test.setname "name with spaces" } e
        assert_match "*Invalid argument*" $e
        assert_equal nobody [r client getname]
        assert_equal nobody [r test.getname]
        r test.setname somebody
        assert_equal somebody [r client getname]
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

    test {rm_call OOM} {
        r config set maxmemory 1
        r config set maxmemory-policy volatile-lru

        # sanity test plain call
        assert_equal {OK} [
            r test.rm_call set x 1
        ]

        # add the M flag
        assert_error {OOM *} {
            r test.rm_call_flags M set x 1

        }

        # test a non deny-oom command
        assert_equal {1} [
            r test.rm_call_flags M get x
        ]

        r config set maxmemory 0
    } {OK} {needs:config-maxmemory}

    test {rm_call OOM Eval} {
        r config set maxmemory 1
        r config set maxmemory-policy volatile-lru

        # use the M flag without allow-oom shebang flag
        assert_error {OOM *} {
            r test.rm_call_flags M eval {#!lua
                redis.call('set','x',1)
                return 1
            } 1 x
        }

        # add the M flag with allow-oom shebang flag
        assert_equal {1} [
            r test.rm_call_flags M eval {#!lua flags=allow-oom
                redis.call('set','x',1)
                return 1
            } 1 x
        ]

        r config set maxmemory 0
    } {OK} {needs:config-maxmemory}

    test {rm_call write flag} {
        # add the W flag
        assert_error {ERR Write command 'set' was called while write is not allowed.} {
            r test.rm_call_flags W set x 1
        }

        # test a non deny-oom command
        r test.rm_call_flags W get x
    } {1}

    test {rm_call EVAL} {
        r test.rm_call eval {
            redis.call('set','x',1)
            return 1
        } 1 x

        assert_error {ERR Write commands are not allowed from read-only scripts.*} {
            r test.rm_call eval {#!lua flags=no-writes
                redis.call('set','x',1)
                return 1
            } 1 x
        }
    }

    test {rm_call EVAL - OOM} {
        r config set maxmemory 1

        assert_error {OOM command not allowed when used memory > 'maxmemory'. script*} {
            r test.rm_call eval {
                redis.call('set','x',1)
                return 1
            } 1 x
        }

        r test.rm_call eval {#!lua flags=no-writes
            redis.call('get','x')
            return 2
        } 1 x

        assert_error {OOM allow-oom flag is not set on the script,*} {
            r test.rm_call eval {#!lua
                redis.call('get','x')
                return 3
            } 1 x
        }

        r test.rm_call eval {
            redis.call('get','x')
            return 4
        } 1 x

        r config set maxmemory 0
    } {OK} {needs:config-maxmemory}

    test "not enough good replicas" {
        r set x "some value"
        r config set min-replicas-to-write 1

        # rm_call in script mode
        assert_error {NOREPLICAS *} {r test.rm_call_flags S set x s}

        assert_equal [
            r test.rm_call eval {#!lua flags=no-writes
                return redis.call('get','x')
            } 1 x
        ] "some value"

        assert_equal [
            r test.rm_call eval {
                return redis.call('get','x')
            } 1 x
        ] "some value"

        assert_error {NOREPLICAS *} {
            r test.rm_call eval {#!lua
                return redis.call('get','x')
            } 1 x
        }

        assert_error {NOREPLICAS *} {
            r test.rm_call eval {
                return redis.call('set','x', 1)
            } 1 x
        }

        r config set min-replicas-to-write 0
    }

    test {rm_call EVAL - read-only replica} {
        r replicaof 127.0.0.1 1

        # rm_call in script mode
        assert_error {READONLY *} {r test.rm_call_flags S set x 1}

        assert_error {READONLY You can't write against a read only replica. script*} {
            r test.rm_call eval {
                redis.call('set','x',1)
                return 1
            } 1 x
        }

        r test.rm_call eval {#!lua flags=no-writes
            redis.call('get','x')
            return 2
        } 1 x

        assert_error {READONLY Can not run script with write flag on readonly replica*} {
            r test.rm_call eval {#!lua
                redis.call('get','x')
                return 3
            } 1 x
        }

        r test.rm_call eval {
            redis.call('get','x')
            return 4
        } 1 x

        r replicaof no one
    } {OK} {needs:config-maxmemory}

    test {rm_call EVAL - stale replica} {
        r replicaof 127.0.0.1 1
        r config set replica-serve-stale-data no

        # rm_call in script mode
        assert_error {MASTERDOWN *} {
            r test.rm_call_flags S get x
        }

        assert_error {MASTERDOWN *} {
            r test.rm_call eval {#!lua flags=no-writes
                redis.call('get','x')
                return 2
            } 1 x
        }

        assert_error {MASTERDOWN *} {
            r test.rm_call eval {
                redis.call('get','x')
                return 4
            } 1 x
        }

        r replicaof no one
        r config set replica-serve-stale-data yes
    } {OK} {needs:config-maxmemory}

    test "rm_call EVAL - failed bgsave prevents writes" {
        r config set rdb-key-save-delay 10000000
        populate 1000
        r set x x
        r bgsave
        set pid1 [get_child_pid 0]
        catch {exec kill -9 $pid1}
        waitForBgsave r

        # make sure a read command succeeds
        assert_equal [r get x] x

        # make sure a write command fails
        assert_error {MISCONF *} {r set x y}

        # rm_call in script mode
        assert_error {MISCONF *} {r test.rm_call_flags S set x 1}

        # repeate with script
        assert_error {MISCONF *} {r test.rm_call eval {
            return redis.call('set','x',1)
            } 1 x
        }
        assert_equal {x} [r test.rm_call eval {
            return redis.call('get','x')
            } 1 x
        ]

        # again with script using shebang
        assert_error {MISCONF *} {r test.rm_call eval {#!lua
            return redis.call('set','x',1)
            } 1 x
        }
        assert_equal {x} [r test.rm_call eval {#!lua flags=no-writes
            return redis.call('get','x')
            } 1 x
        ]

        r config set rdb-key-save-delay 0
        r bgsave
        waitForBgsave r

        # server is writable again
        r set x y
    } {OK}

    test "Unload the module - misc" {
        assert_equal {OK} [r module unload misc]
    }
}
