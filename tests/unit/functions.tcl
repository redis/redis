proc get_function_code {args} {
    return [format "#!%s name=%s\nredis.register_function('%s', function(KEYS, ARGV)\n %s \nend)" [lindex $args 0] [lindex $args 1] [lindex $args 2] [lindex $args 3]]
}

proc get_no_writes_function_code {args} {
    return [format "#!%s name=%s\nredis.register_function{function_name='%s', callback=function(KEYS, ARGV)\n %s \nend, flags={'no-writes'}}" [lindex $args 0] [lindex $args 1] [lindex $args 2] [lindex $args 3]]
}

start_server {tags {"scripting"}} {
    test {FUNCTION - Basic usage} {
        r function load [get_function_code LUA test test {return 'hello'}]
        r fcall test 0
    } {hello}

    test {FUNCTION - Load with unknown argument} {
        catch {
            r function load foo bar [get_function_code LUA test test {return 'hello'}]
        } e
        set _ $e
    } {*Unknown option given*}

    test {FUNCTION - Create an already exiting library raise error} {
        catch {
            r function load [get_function_code LUA test test {return 'hello1'}]
        } e
        set _ $e
    } {*already exists*}

    test {FUNCTION - Create an already exiting library raise error (case insensitive)} {
        catch {
            r function load [get_function_code LUA test test {return 'hello1'}]
        } e
        set _ $e
    } {*already exists*}

    test {FUNCTION - Create a library with wrong name format} {
        catch {
            r function load [get_function_code LUA {bad\0foramat} test {return 'hello1'}]
        } e
        set _ $e
    } {*Library names can only contain letters, numbers, or underscores(_)*}

    test {FUNCTION - Create library with unexisting engine} {
        catch {
            r function load [get_function_code bad_engine test test {return 'hello1'}]
        } e
        set _ $e
    } {*Engine 'bad_engine' not found*}

    test {FUNCTION - Test uncompiled script} {
        catch {
            r function load replace [get_function_code LUA test test {bad script}]
        } e
        set _ $e
    } {*Error compiling function*}

    test {FUNCTION - test replace argument} {
        r function load REPLACE [get_function_code LUA test test {return 'hello1'}]
        r fcall test 0
    } {hello1}

    test {FUNCTION - test function case insensitive} {
        r fcall TEST 0
    } {hello1}

    test {FUNCTION - test replace argument with failure keeps old libraries} {
        catch {r function load REPLACE [get_function_code LUA test test {error}]} e
        assert_match {ERR Error compiling function*} $e
        r fcall test 0
    } {hello1}

    test {FUNCTION - test function delete} {
        r function delete test
        catch {
            r fcall test 0
        } e
        set _ $e
    } {*Function not found*}

    test {FUNCTION - test fcall bad arguments} {
        r function load [get_function_code LUA test test {return 'hello'}]
        catch {
            r fcall test bad_arg
        } e
        set _ $e
    } {*Bad number of keys provided*}

    test {FUNCTION - test fcall bad number of keys arguments} {
        catch {
            r fcall test 10 key1
        } e
        set _ $e
    } {*Number of keys can't be greater than number of args*}

    test {FUNCTION - test fcall negative number of keys} {
        catch {
            r fcall test -1 key1
        } e
        set _ $e
    } {*Number of keys can't be negative*}

    test {FUNCTION - test delete on not exiting library} {
        catch {
            r function delete test1
        } e
        set _ $e
    } {*Library not found*}

    test {FUNCTION - test function kill when function is not running} {
        catch {
            r function kill
        } e
        set _ $e
    } {*No scripts in execution*}

    test {FUNCTION - test wrong subcommand} {
        catch {
            r function bad_subcommand
        } e
        set _ $e
    } {*unknown subcommand*}

    test {FUNCTION - test loading from rdb} {
        r debug reload
        r fcall test 0
    } {hello} {needs:debug}

    test {FUNCTION - test debug reload different options} {
        catch {r debug reload noflush} e
        assert_match "*Error trying to load the RDB*" $e
        r debug reload noflush merge
        r function list
    } {{library_name test engine LUA functions {{name test description {} flags {}}}}} {needs:debug}

    test {FUNCTION - test debug reload with nosave and noflush} {
        r function delete test
        r set x 1
        r function load [get_function_code LUA test1 test1 {return 'hello'}]
        r debug reload
        r function load [get_function_code LUA test2 test2 {return 'hello'}]
        r debug reload nosave noflush merge
        assert_equal [r fcall test1 0] {hello}
        assert_equal [r fcall test2 0] {hello}
    } {} {needs:debug}

    test {FUNCTION - test flushall and flushdb do not clean functions} {
        r function flush
        r function load REPLACE [get_function_code lua test test {return redis.call('set', 'x', '1')}]
        r flushall
        r flushdb
        r function list
    } {{library_name test engine LUA functions {{name test description {} flags {}}}}}

    test {FUNCTION - test function dump and restore} {
        r function flush
        r function load [get_function_code lua test test {return 'hello'}]
        set e [r function dump]
        r function delete test
        assert_match {} [r function list]
        r function restore $e
        r function list
    } {{library_name test engine LUA functions {{name test description {} flags {}}}}}

    test {FUNCTION - test function dump and restore with flush argument} {
        set e [r function dump]
        r function flush
        assert_match {} [r function list]
        r function restore $e FLUSH
        r function list
    } {{library_name test engine LUA functions {{name test description {} flags {}}}}}

    test {FUNCTION - test function dump and restore with append argument} {
        set e [r function dump]
        r function flush
        assert_match {} [r function list]
        r function load [get_function_code lua test test {return 'hello1'}]
        catch {r function restore $e APPEND} err
        assert_match {*already exists*} $err
        r function flush
        r function load [get_function_code lua test1 test1 {return 'hello1'}]
        r function restore $e APPEND
        assert_match {hello} [r fcall test 0]
        assert_match {hello1} [r fcall test1 0]
    }

    test {FUNCTION - test function dump and restore with replace argument} {
        r function flush
        r function load [get_function_code LUA test test {return 'hello'}]
        set e [r function dump]
        r function flush
        assert_match {} [r function list]
        r function load [get_function_code lua test test {return 'hello1'}]
        assert_match {hello1} [r fcall test 0]
        r function restore $e REPLACE
        assert_match {hello} [r fcall test 0]
    }

    test {FUNCTION - test function restore with bad payload do not drop existing functions} {
        r function flush
        r function load [get_function_code LUA test test {return 'hello'}]
        catch {r function restore bad_payload} e
        assert_match {*payload version or checksum are wrong*} $e
        r function list
    } {{library_name test engine LUA functions {{name test description {} flags {}}}}}

    test {FUNCTION - test function restore with wrong number of arguments} {
        catch {r function restore arg1 args2 arg3} e
        set _ $e
    } {*unknown subcommand or wrong number of arguments for 'restore'. Try FUNCTION HELP.}

    test {FUNCTION - test fcall_ro with write command} {
        r function load REPLACE [get_no_writes_function_code lua test test {return redis.call('set', 'x', '1')}]
        catch { r fcall_ro test 1 x } e
        set _ $e
    } {*Write commands are not allowed from read-only scripts*}

    test {FUNCTION - test fcall_ro with read only commands} {
        r function load REPLACE [get_no_writes_function_code lua test test {return redis.call('get', 'x')}]
        r set x 1
        r fcall_ro test 1 x
    } {1}

    test {FUNCTION - test keys and argv} {
        r function load REPLACE [get_function_code lua test test {return redis.call('set', KEYS[1], ARGV[1])}]
        r fcall test 1 x foo
        r get x
    } {foo}

    test {FUNCTION - test command get keys on fcall} {
        r COMMAND GETKEYS fcall test 1 x foo
    } {x}

    test {FUNCTION - test command get keys on fcall_ro} {
        r COMMAND GETKEYS fcall_ro test 1 x foo
    } {x}

    test {FUNCTION - test function kill} {
        set rd [redis_deferring_client]
        r config set busy-reply-threshold 10
        r function load REPLACE [get_function_code lua test test {local a = 1 while true do a = a + 1 end}]
        $rd fcall test 0
        after 200
        catch {r ping} e
        assert_match {BUSY*} $e
        assert_match {running_script {name test command {fcall test 0} duration_ms *} engines {*}} [r FUNCTION STATS]
        r function kill
        after 200 ; # Give some time to Lua to call the hook again...
        assert_equal [r ping] "PONG"
    }

    test {FUNCTION - test script kill not working on function} {
        set rd [redis_deferring_client]
        r config set busy-reply-threshold 10
        r function load REPLACE [get_function_code lua test test {local a = 1 while true do a = a + 1 end}]
        $rd fcall test 0
        after 200
        catch {r ping} e
        assert_match {BUSY*} $e
        catch {r script kill} e
        assert_match {BUSY*} $e
        r function kill
        after 200 ; # Give some time to Lua to call the hook again...
        assert_equal [r ping] "PONG"
    }

    test {FUNCTION - test function kill not working on eval} {
        set rd [redis_deferring_client]
        r config set busy-reply-threshold 10
        $rd eval {local a = 1 while true do a = a + 1 end} 0
        after 200
        catch {r ping} e
        assert_match {BUSY*} $e
        catch {r function kill} e
        assert_match {BUSY*} $e
        r script kill
        after 200 ; # Give some time to Lua to call the hook again...
        assert_equal [r ping] "PONG"
    }

    test {FUNCTION - test function flush} {
        r function load REPLACE [get_function_code lua test test {local a = 1 while true do a = a + 1 end}]
        assert_match {{library_name test engine LUA functions {{name test description {} flags {}}}}} [r function list]
        r function flush
        assert_match {} [r function list]

        r function load REPLACE [get_function_code lua test test {local a = 1 while true do a = a + 1 end}]
        assert_match {{library_name test engine LUA functions {{name test description {} flags {}}}}} [r function list]
        r function flush async
        assert_match {} [r function list]

        r function load REPLACE [get_function_code lua test test {local a = 1 while true do a = a + 1 end}]
        assert_match {{library_name test engine LUA functions {{name test description {} flags {}}}}} [r function list]
        r function flush sync
        assert_match {} [r function list]
    }

    test {FUNCTION - async function flush rebuilds Lua VM without causing race condition between main and lazyfree thread} {
        # LAZYFREE_THRESHOLD is 64
        for {set i 0} {$i < 1000} {incr i} {
            r function load [get_function_code lua test$i test$i {local a = 1 while true do a = a + 1 end}]
        }
        assert_morethan [s used_memory_vm_functions] 100000
        r config resetstat
        r function flush async
        assert_lessthan [s used_memory_vm_functions] 40000

        # Wait for the completion of lazy free for both functions and engines.
        set start_time [clock seconds]
        while {1} {
            # Tests for race conditions between async function flushes and main thread Lua VM operations.
            r function load REPLACE [get_function_code lua test test {local a = 1 while true do a = a + 1 end}]
            if {[s lazyfreed_objects] == 1001 || [expr {[clock seconds] - $start_time}] > 5} {
                break
            }
        }
        if {[s lazyfreed_objects] != 1001} {
            error "Timeout or unexpected number of lazyfreed_objects: [s lazyfreed_objects]"
        }
        assert_match {{library_name test engine LUA functions {{name test description {} flags {}}}}} [r function list]
    }

    test {FUNCTION - test function wrong argument} {
        catch {r function flush bad_arg} e
        assert_match {*only supports SYNC|ASYNC*} $e

        catch {r function flush sync extra_arg} e
        assert_match {*unknown subcommand or wrong number of arguments for 'flush'. Try FUNCTION HELP.} $e
    }
}

start_server {tags {"scripting repl external:skip"}} {
    start_server {} {
        test "Connect a replica to the master instance" {
            r -1 slaveof [srv 0 host] [srv 0 port]
            wait_for_condition 150 100 {
                [s -1 role] eq {slave} &&
                [string match {*master_link_status:up*} [r -1 info replication]]
            } else {
                fail "Can't turn the instance into a replica"
            }
        }

        test {FUNCTION - creation is replicated to replica} {
            r function load [get_no_writes_function_code LUA test test {return 'hello'}]
            wait_for_condition 150 100 {    
                [r -1 function list] eq {{library_name test engine LUA functions {{name test description {} flags no-writes}}}}
            } else {
                fail "Failed waiting for function to replicate to replica"
            }
        }

        test {FUNCTION - call on replica} {
            r -1 fcall test 0
        } {hello}

        test {FUNCTION - restore is replicated to replica} {
            set e [r function dump]

            r function delete test
            wait_for_condition 150 100 {
                [r -1 function list] eq {}
            } else {
                fail "Failed waiting for function to replicate to replica"
            }

            assert_equal [r function restore $e] {OK}

            wait_for_condition 150 100 {
                [r -1 function list] eq {{library_name test engine LUA functions {{name test description {} flags no-writes}}}}
            } else {
                fail "Failed waiting for function to replicate to replica"
            }
        }

        test {FUNCTION - delete is replicated to replica} {
            r function delete test
            wait_for_condition 150 100 {
                [r -1 function list] eq {}
            } else {
                fail "Failed waiting for function to replicate to replica"
            }
        }

        test {FUNCTION - flush is replicated to replica} {
            r function load [get_function_code LUA test test {return 'hello'}]
            wait_for_condition 150 100 {
                [r -1 function list] eq {{library_name test engine LUA functions {{name test description {} flags {}}}}}
            } else {
                fail "Failed waiting for function to replicate to replica"
            }
            r function flush
            wait_for_condition 150 100 {
                [r -1 function list] eq {}
            } else {
                fail "Failed waiting for function to replicate to replica"
            }
        }

        test "Disconnecting the replica from master instance" {
            r -1 slaveof no one
            # creating a function after disconnect to make sure function
            # is replicated on rdb phase
            r function load [get_no_writes_function_code LUA test test {return 'hello'}]

            # reconnect the replica
            r -1 slaveof [srv 0 host] [srv 0 port]
            wait_for_condition 150 100 {
                [s -1 role] eq {slave} &&
                [string match {*master_link_status:up*} [r -1 info replication]]
            } else {
                fail "Can't turn the instance into a replica"
            }
        }

        test "FUNCTION - test replication to replica on rdb phase" {
            r -1 fcall test 0
        } {hello}

        test "FUNCTION - test replication to replica on rdb phase info command" {
            r -1 function list
        } {{library_name test engine LUA functions {{name test description {} flags no-writes}}}}

        test "FUNCTION - create on read only replica" {
            catch {
                r -1 function load [get_function_code LUA test test {return 'hello'}]
            } e
            set _ $e
        } {*can't write against a read only replica*}

        test "FUNCTION - delete on read only replica" {
            catch {
                r -1 function delete test
            } e
            set _ $e
        } {*can't write against a read only replica*}

        test "FUNCTION - function effect is replicated to replica" {
            r function load REPLACE [get_function_code LUA test test {return redis.call('set', 'x', '1')}]
            r fcall test 1 x
            assert {[r get x] eq {1}}
            wait_for_condition 150 100 {
                [r -1 get x] eq {1}
            } else {
                fail "Failed waiting function effect to be replicated to replica"
            }
        }

        test "FUNCTION - modify key space of read only replica" {
            catch {
                r -1 fcall test 1 x
            } e
            set _ $e
        } {READONLY You can't write against a read only replica.}
    }
}

test {FUNCTION can processes create, delete and flush commands in AOF when doing "debug loadaof" in read-only slaves} {
    start_server {} {
        r config set appendonly yes
        waitForBgrewriteaof r
        r FUNCTION LOAD "#!lua name=test\nredis.register_function('test', function() return 'hello' end)"
        r config set slave-read-only yes
        r slaveof 127.0.0.1 0
        r debug loadaof
        r slaveof no one
        assert_equal [r function list] {{library_name test engine LUA functions {{name test description {} flags {}}}}}

        r FUNCTION DELETE test

        r slaveof 127.0.0.1 0
        r debug loadaof
        r slaveof no one
        assert_equal [r function list] {}

        r FUNCTION LOAD "#!lua name=test\nredis.register_function('test', function() return 'hello' end)"
        r FUNCTION FLUSH

        r slaveof 127.0.0.1 0
        r debug loadaof
        r slaveof no one
        assert_equal [r function list] {}
    }
} {} {needs:debug external:skip}

start_server {tags {"scripting"}} {
    test {LIBRARIES - test shared function can access default globals} {
        r function load {#!lua name=lib1
            local function ping()
                return redis.call('ping')
            end
            redis.register_function(
                'f1',
                function(keys, args)
                    return ping()
                end
            )
        }
        r fcall f1 0
    } {PONG}

    test {LIBRARIES - usage and code sharing} {
        r function load REPLACE {#!lua name=lib1
            local function add1(a)
                return a + 1
            end
            redis.register_function(
                'f1',
                function(keys, args)
                    return add1(1)
                end
            )
            redis.register_function(
                'f2',
                function(keys, args)
                    return add1(2)
                end
            )
        }
        assert_equal [r fcall f1 0] {2}
        assert_equal [r fcall f2 0] {3}
        r function list
    } {{library_name lib1 engine LUA functions {*}}}

    test {LIBRARIES - test registration failure revert the entire load} {
        catch {
            r function load replace {#!lua name=lib1
                local function add1(a)
                    return a + 2
                end
                redis.register_function(
                    'f1',
                    function(keys, args)
                        return add1(1)
                    end
                )
                redis.register_function(
                    'f2',
                    'not a function'
                )
            }
        } e
        assert_match {*second argument to redis.register_function must be a function*} $e
        assert_equal [r fcall f1 0] {2}
        assert_equal [r fcall f2 0] {3}
    }

    test {LIBRARIES - test registration function name collision} {
        catch {
            r function load replace {#!lua name=lib2
                redis.register_function(
                    'f1',
                    function(keys, args)
                        return 1
                    end
                )
            }
        } e
        assert_match {*Function f1 already exists*} $e
        assert_equal [r fcall f1 0] {2}
        assert_equal [r fcall f2 0] {3}
    }

    test {LIBRARIES - test registration function name collision on same library} {
        catch {
            r function load replace {#!lua name=lib2
                redis.register_function(
                    'f1',
                    function(keys, args)
                        return 1
                    end
                )
                redis.register_function(
                    'f1',
                    function(keys, args)
                        return 1
                    end
                )
            }
        } e
        set _ $e
    } {*Function already exists in the library*}

    test {LIBRARIES - test registration with no argument} {
        catch {
            r function load replace {#!lua name=lib2
                redis.register_function()
            }
        } e
        set _ $e
    } {*wrong number of arguments to redis.register_function*}

    test {LIBRARIES - test registration with only name} {
        catch {
            r function load replace {#!lua name=lib2
                redis.register_function('f1')
            }
        } e
        set _ $e
    } {*calling redis.register_function with a single argument is only applicable to Lua table*}

    test {LIBRARIES - test registration with to many arguments} {
        catch {
            r function load replace {#!lua name=lib2
                redis.register_function('f1', function() return 1 end, {}, 'description', 'extra arg')
            }
        } e
        set _ $e
    } {*wrong number of arguments to redis.register_function*}

    test {LIBRARIES - test registration with no string name} {
        catch {
            r function load replace {#!lua name=lib2
                redis.register_function(nil, function() return 1 end)
            }
        } e
        set _ $e
    } {*first argument to redis.register_function must be a string*}

    test {LIBRARIES - test registration with wrong name format} {
        catch {
            r function load replace {#!lua name=lib2
                redis.register_function('test\0test', function() return 1 end)
            }
        } e
        set _ $e
    } {*Library names can only contain letters, numbers, or underscores(_) and must be at least one character long*}

    test {LIBRARIES - test registration with empty name} {
        catch {
            r function load replace {#!lua name=lib2
                redis.register_function('', function() return 1 end)
            }
        } e
        set _ $e
    } {*Library names can only contain letters, numbers, or underscores(_) and must be at least one character long*}

    test {LIBRARIES - math.random from function load} {
        catch {
            r function load replace {#!lua name=lib2
                return math.random()
            }
        } e
        set _ $e
    } {*attempted to access nonexistent global variable 'math'*}

    test {LIBRARIES - redis.call from function load} {
        catch {
            r function load replace {#!lua name=lib2
                return redis.call('ping')
            }
        } e
        set _ $e
    } {*attempted to access nonexistent global variable 'call'*}

    test {LIBRARIES - redis.setresp from function load} {
        catch {
            r function load replace {#!lua name=lib2
                return redis.setresp(3)
            }
        } e
        set _ $e
    } {*attempted to access nonexistent global variable 'setresp'*}

    test {LIBRARIES - redis.set_repl from function load} {
        catch {
            r function load replace {#!lua name=lib2
                return redis.set_repl(redis.REPL_NONE)
            }
        } e
        set _ $e
    } {*attempted to access nonexistent global variable 'set_repl'*}

    test {LIBRARIES - redis.acl_check_cmd from function load} {
        catch {
            r function load replace {#!lua name=lib2
                return redis.acl_check_cmd('set','xx',1)
            }
        } e
        set _ $e
    } {*attempted to access nonexistent global variable 'acl_check_cmd'*}

    test {LIBRARIES - malicious access test} {
        # the 'library' API is not exposed inside a
        # function context and the 'redis' API is not
        # expose on the library registration context.
        # But a malicious user might find a way to hack it
        # (as demonstrated in this test). This is why we
        # have another level of protection on the C
        # code itself and we want to test it and verify
        # that it works properly.
        r function load replace {#!lua name=lib1
            local lib = redis
            lib.register_function('f1', function ()
                lib.redis = redis
                lib.math = math
                return {ok='OK'}
            end)

            lib.register_function('f2', function ()
                lib.register_function('f1', function ()
                    lib.redis = redis
                    lib.math = math
                    return {ok='OK'}
                end)
            end)
        }
        catch {[r fcall f1 0]} e
        assert_match {*Attempt to modify a readonly table*} $e

        catch {[r function load {#!lua name=lib2
            redis.math.random()
        }]} e
        assert_match {*Script attempted to access nonexistent global variable 'math'*} $e

        catch {[r function load {#!lua name=lib2
            redis.redis.call('ping')
        }]} e
        assert_match {*Script attempted to access nonexistent global variable 'redis'*} $e

        catch {[r fcall f2 0]} e
        assert_match {*can only be called on FUNCTION LOAD command*} $e
    }

    test {LIBRARIES - delete removed all functions on library} {
        r function delete lib1
        r function list
    } {}

    test {LIBRARIES - register function inside a function} {
        r function load {#!lua name=lib
            redis.register_function(
                'f1',
                function(keys, args)
                    redis.register_function(
                        'f2',
                        function(key, args)
                            return 2
                        end
                    )
                    return 1
                end
            )
        }
        catch {r fcall f1 0} e
        set _ $e
    } {*attempt to call field 'register_function' (a nil value)*}

    test {LIBRARIES - register library with no functions} {
        r function flush
        catch {
            r function load {#!lua name=lib
                return 1
            }
        } e
        set _ $e
    } {*No functions registered*}

    test {LIBRARIES - load timeout} {
        catch {
            r function load {#!lua name=lib
                local a = 1
                while 1 do a = a + 1 end
            }
        } e
        set _ $e
    } {*FUNCTION LOAD timeout*}

    test {LIBRARIES - verify global protection on the load run} {
        catch {
            r function load {#!lua name=lib
                a = 1
            }
        } e
        set _ $e
    } {*Attempt to modify a readonly table*}

    test {LIBRARIES - named arguments} {
        r function load {#!lua name=lib
            redis.register_function{
                function_name='f1',
                callback=function()
                    return 'hello'
                end,
                description='some desc'
            }
        }
        r function list
    } {{library_name lib engine LUA functions {{name f1 description {some desc} flags {}}}}}

    test {LIBRARIES - named arguments, bad function name} {
        catch {
            r function load replace {#!lua name=lib
                redis.register_function{
                    function_name=function() return 1 end,
                    callback=function()
                        return 'hello'
                    end,
                    description='some desc'
                }
            }
        } e
        set _ $e
    } {*function_name argument given to redis.register_function must be a string*}

    test {LIBRARIES - named arguments, bad callback type} {
        catch {
            r function load replace {#!lua name=lib
                redis.register_function{
                    function_name='f1',
                    callback='bad',
                    description='some desc'
                }
            }
        } e
        set _ $e
    } {*callback argument given to redis.register_function must be a function*}

    test {LIBRARIES - named arguments, bad description} {
        catch {
            r function load replace {#!lua name=lib
                redis.register_function{
                    function_name='f1',
                    callback=function()
                        return 'hello'
                    end,
                    description=function() return 1 end
                }
            }
        } e
        set _ $e
    } {*description argument given to redis.register_function must be a string*}

    test {LIBRARIES - named arguments, unknown argument} {
        catch {
            r function load replace {#!lua name=lib
                redis.register_function{
                    function_name='f1',
                    callback=function()
                        return 'hello'
                    end,
                    description='desc',
                    some_unknown='unknown'
                }
            }
        } e
        set _ $e
    } {*unknown argument given to redis.register_function*}

    test {LIBRARIES - named arguments, missing function name} {
        catch {
            r function load replace {#!lua name=lib
                redis.register_function{
                    callback=function()
                        return 'hello'
                    end,
                    description='desc'
                }
            }
        } e
        set _ $e
    } {*redis.register_function must get a function name argument*}

    test {LIBRARIES - named arguments, missing callback} {
        catch {
            r function load replace {#!lua name=lib
                redis.register_function{
                    function_name='f1',
                    description='desc'
                }
            }
        } e
        set _ $e
    } {*redis.register_function must get a callback argument*}

    test {FUNCTION - test function restore with function name collision} {
        r function flush
        r function load {#!lua name=lib1
            local function add1(a)
                return a + 1
            end
            redis.register_function(
                'f1',
                function(keys, args)
                    return add1(1)
                end
            )
            redis.register_function(
                'f2',
                function(keys, args)
                    return add1(2)
                end
            )
            redis.register_function(
                'f3',
                function(keys, args)
                    return add1(3)
                end
            )
        }
        set e [r function dump]
        r function flush

        # load a library with different name but with the same function name
        r function load {#!lua name=lib1
            redis.register_function(
                'f6',
                function(keys, args)
                    return 7
                end
            )
        }
        r function load {#!lua name=lib2
            local function add1(a)
                return a + 1
            end
            redis.register_function(
                'f4',
                function(keys, args)
                    return add1(4)
                end
            )
            redis.register_function(
                'f5',
                function(keys, args)
                    return add1(5)
                end
            )
            redis.register_function(
                'f3',
                function(keys, args)
                    return add1(3)
                end
            )
        }

        catch {r function restore $e} error
        assert_match {*Library lib1 already exists*} $error
        assert_equal [r fcall f3 0] {4}
        assert_equal [r fcall f4 0] {5}
        assert_equal [r fcall f5 0] {6}
        assert_equal [r fcall f6 0] {7}

        catch {r function restore $e replace} error
        assert_match {*Function f3 already exists*} $error
        assert_equal [r fcall f3 0] {4}
        assert_equal [r fcall f4 0] {5}
        assert_equal [r fcall f5 0] {6}
        assert_equal [r fcall f6 0] {7}
    }

    test {FUNCTION - test function list with code} {
        r function flush
        r function load {#!lua name=library1
            redis.register_function('f6', function(keys, args) return 7 end)
        }
        r function list withcode
    } {{library_name library1 engine LUA functions {{name f6 description {} flags {}}} library_code {*redis.register_function('f6', function(keys, args) return 7 end)*}}}

    test {FUNCTION - test function list with pattern} {
        r function load {#!lua name=lib1
            redis.register_function('f7', function(keys, args) return 7 end)
        }
        r function list libraryname library*
    } {{library_name library1 engine LUA functions {{name f6 description {} flags {}}}}}

    test {FUNCTION - test function list wrong argument} {
        catch {r function list bad_argument} e
        set _ $e
    } {*Unknown argument bad_argument*}

    test {FUNCTION - test function list with bad argument to library name} {
        catch {r function list libraryname} e
        set _ $e
    } {*library name argument was not given*}

    test {FUNCTION - test function list withcode multiple times} {
        catch {r function list withcode withcode} e
        set _ $e
    } {*Unknown argument withcode*}

    test {FUNCTION - test function list libraryname multiple times} {
        catch {r function list withcode libraryname foo libraryname foo} e
        set _ $e
    } {*Unknown argument libraryname*}

    test {FUNCTION - verify OOM on function load and function restore} {
        r function flush
        r function load replace {#!lua name=test
            redis.register_function('f1', function() return 1 end)
        }
        set payload [r function dump]
        r config set maxmemory 1

        r function flush
        catch {r function load replace {#!lua name=test
            redis.register_function('f1', function() return 1 end)
        }} e
        assert_match {*command not allowed when used memory*} $e

        r function flush
        catch {r function restore $payload} e
        assert_match {*command not allowed when used memory*} $e

        r config set maxmemory 0
    } {OK} {needs:config-maxmemory}

    test {FUNCTION - verify allow-omm allows running any command} {
        r FUNCTION load replace {#!lua name=f1
            redis.register_function{
                function_name='f1',
                callback=function() return redis.call('set', 'x', '1') end,
                flags={'allow-oom'}
            }
        }

        r config set maxmemory 1

        assert_match {OK} [r fcall f1 1 x]
        assert_match {1} [r get x]

        r config set maxmemory 0
    } {OK} {needs:config-maxmemory}
}

start_server {tags {"scripting"}} {
    test {FUNCTION - wrong flags type named arguments} {
        catch {r function load replace {#!lua name=test
            redis.register_function{
                function_name = 'f1',
                callback = function() return 1 end,
                flags = 'bad flags type'
            }
        }} e
        set _ $e
    } {*flags argument to redis.register_function must be a table representing function flags*}

    test {FUNCTION - wrong flag type} {
        catch {r function load replace {#!lua name=test
            redis.register_function{
                function_name = 'f1',
                callback = function() return 1 end,
                flags = {function() return 1 end}
            }
        }} e
        set _ $e
    } {*unknown flag given*}

    test {FUNCTION - unknown flag} {
        catch {r function load replace {#!lua name=test
            redis.register_function{
                function_name = 'f1',
                callback = function() return 1 end,
                flags = {'unknown'}
            }
        }} e
        set _ $e
    } {*unknown flag given*}

    test {FUNCTION - write script on fcall_ro} {
        r function load replace {#!lua name=test
            redis.register_function{
                function_name = 'f1',
                callback = function() return redis.call('set', 'x', 1) end
            }
        }
        catch {r fcall_ro f1 1 x} e
        set _ $e
    } {*Can not execute a script with write flag using \*_ro command*}

    test {FUNCTION - write script with no-writes flag} {
        r function load replace {#!lua name=test
            redis.register_function{
                function_name = 'f1',
                callback = function() return redis.call('set', 'x', 1) end,
                flags = {'no-writes'}
            }
        }
        catch {r fcall f1 1 x} e
        set _ $e
    } {*Write commands are not allowed from read-only scripts*}

    test {FUNCTION - deny oom} {
        r FUNCTION load replace {#!lua name=test
            redis.register_function('f1', function() return redis.call('set', 'x', '1') end) 
        }

        r config set maxmemory 1

        catch {[r fcall f1 1 x]} e
        assert_match {OOM *when used memory > 'maxmemory'*} $e

        r config set maxmemory 0
    } {OK} {needs:config-maxmemory}

    test {FUNCTION - deny oom on no-writes function} {
        r FUNCTION load replace {#!lua name=test
            redis.register_function{function_name='f1', callback=function() return 'hello' end, flags={'no-writes'}}
        }

        r config set maxmemory 1

        assert_equal [r fcall f1 1 k] hello
        assert_equal [r fcall_ro f1 1 k] hello

        r config set maxmemory 0
    } {OK} {needs:config-maxmemory}

    test {FUNCTION - allow stale} {
        r FUNCTION load replace {#!lua name=test
            redis.register_function{function_name='f1', callback=function() return 'hello' end, flags={'no-writes'}}
            redis.register_function{function_name='f2', callback=function() return 'hello' end, flags={'allow-stale', 'no-writes'}}
            redis.register_function{function_name='f3', callback=function() return redis.call('get', 'x') end, flags={'allow-stale', 'no-writes'}}
            redis.register_function{function_name='f4', callback=function() return redis.call('info', 'server') end, flags={'allow-stale', 'no-writes'}}
        }
        
        r config set replica-serve-stale-data no
        r replicaof 127.0.0.1 1

        catch {[r fcall f1 0]} e
        assert_match {MASTERDOWN *} $e

        assert_equal {hello} [r fcall f2 0]

        catch {[r fcall f3 1 x]} e
        assert_match {ERR *Can not execute the command on a stale replica*} $e

        assert_match {*redis_version*} [r fcall f4 0]

        r replicaof no one
        r config set replica-serve-stale-data yes
        set _ {}
    } {} {external:skip}

    test {FUNCTION - redis version api} {
        r FUNCTION load replace {#!lua name=test
            local version = redis.REDIS_VERSION_NUM

            redis.register_function{function_name='get_version_v1', callback=function()
              return string.format('%s.%s.%s',
                                    bit.band(bit.rshift(version, 16), 0x000000ff),
                                    bit.band(bit.rshift(version, 8), 0x000000ff),
                                    bit.band(version, 0x000000ff))
            end}
            redis.register_function{function_name='get_version_v2', callback=function() return redis.REDIS_VERSION end}
        }

        catch {[r fcall f1 0]} e
        assert_equal  [r fcall get_version_v1 0] [r fcall get_version_v2 0]
    }

    test {FUNCTION - function stats} {
        r FUNCTION FLUSH

        r FUNCTION load {#!lua name=test1
            redis.register_function('f1', function() return 1 end)
            redis.register_function('f2', function() return 1 end)
        }

        r FUNCTION load {#!lua name=test2
            redis.register_function('f3', function() return 1 end)
        }

        r function stats
    } {running_script {} engines {LUA {libraries_count 2 functions_count 3}}}

    test {FUNCTION - function stats reloaded correctly from rdb} {
        r debug reload
        r function stats
    } {running_script {} engines {LUA {libraries_count 2 functions_count 3}}} {needs:debug}

    test {FUNCTION - function stats delete library} {
        r function delete test1
        r function stats
    } {running_script {} engines {LUA {libraries_count 1 functions_count 1}}}

    test {FUNCTION - test function stats on loading failure} {
        r FUNCTION FLUSH

        r FUNCTION load {#!lua name=test1
            redis.register_function('f1', function() return 1 end)
            redis.register_function('f2', function() return 1 end)
        }

        catch {r FUNCTION load {#!lua name=test1
            redis.register_function('f3', function() return 1 end)
        }} e
        assert_match "*Library 'test1' already exists*" $e
        

        r function stats
    } {running_script {} engines {LUA {libraries_count 1 functions_count 2}}}

    test {FUNCTION - function stats cleaned after flush} {
        r function flush
        r function stats
    } {running_script {} engines {LUA {libraries_count 0 functions_count 0}}}

    test {FUNCTION - function test empty engine} {
         catch {r function load replace {#! name=test
            redis.register_function('foo', function() return 1 end)
        }} e
        set _ $e
    } {ERR Engine '' not found}

    test {FUNCTION - function test unknown metadata value} {
         catch {r function load replace {#!lua name=test foo=bar
            redis.register_function('foo', function() return 1 end)
        }} e
        set _ $e
    } {ERR Invalid metadata value given: foo=bar}

    test {FUNCTION - function test no name} {
         catch {r function load replace {#!lua
            redis.register_function('foo', function() return 1 end)
        }} e
        set _ $e
    } {ERR Library name was not given}

    test {FUNCTION - function test multiple names} {
         catch {r function load replace {#!lua name=foo name=bar
            redis.register_function('foo', function() return 1 end)
        }} e
        set _ $e
    } {ERR Invalid metadata value, name argument was given multiple times}

    test {FUNCTION - function test name with quotes} {
        r function load replace {#!lua name="foo"
            redis.register_function('foo', function() return 1 end)
        }
    } {foo}

    test {FUNCTION - trick global protection 1} {
        r FUNCTION FLUSH

        r FUNCTION load {#!lua name=test1
            redis.register_function('f1', function() 
                mt = getmetatable(_G)
                original_globals = mt.__index
                original_globals['redis'] = function() return 1 end
            end)
        }

        catch {[r fcall f1 0]} e
        set _ $e
    } {*Attempt to modify a readonly table*}

    test {FUNCTION - test getmetatable on script load} {
        r FUNCTION FLUSH

        catch {
            r FUNCTION load {#!lua name=test1
                mt = getmetatable(_G)
            }
        } e

        set _ $e
    } {*Script attempted to access nonexistent global variable 'getmetatable'*}

}
