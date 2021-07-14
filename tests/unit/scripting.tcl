start_server {tags {"scripting"}} {
    test {EVAL - Does Lua interpreter replies to our requests?} {
        r eval {return 'hello'} 0
    } {hello}

    test {EVAL - Lua integer -> Redis protocol type conversion} {
        r eval {return 100.5} 0
    } {100}

    test {EVAL - Lua string -> Redis protocol type conversion} {
        r eval {return 'hello world'} 0
    } {hello world}

    test {EVAL - Lua true boolean -> Redis protocol type conversion} {
        r eval {return true} 0
    } {1}

    test {EVAL - Lua false boolean -> Redis protocol type conversion} {
        r eval {return false} 0
    } {}

    test {EVAL - Lua status code reply -> Redis protocol type conversion} {
        r eval {return {ok='fine'}} 0
    } {fine}

    test {EVAL - Lua error reply -> Redis protocol type conversion} {
        catch {
            r eval {return {err='this is an error'}} 0
        } e
        set _ $e
    } {this is an error}

    test {EVAL - Lua table -> Redis protocol type conversion} {
        r eval {return {1,2,3,'ciao',{1,2}}} 0
    } {1 2 3 ciao {1 2}}

    test {EVAL - Are the KEYS and ARGV arrays populated correctly?} {
        r eval {return {KEYS[1],KEYS[2],ARGV[1],ARGV[2]}} 2 a{t} b{t} c{t} d{t}
    } {a{t} b{t} c{t} d{t}}

    test {EVAL - is Lua able to call Redis API?} {
        r set mykey myval
        r eval {return redis.call('get',KEYS[1])} 1 mykey
    } {myval}

    test {EVALSHA - Can we call a SHA1 if already defined?} {
        r evalsha fd758d1589d044dd850a6f05d52f2eefd27f033f 1 mykey
    } {myval}

    test {EVALSHA - Can we call a SHA1 in uppercase?} {
        r evalsha FD758D1589D044DD850A6F05D52F2EEFD27F033F 1 mykey
    } {myval}

    test {EVALSHA - Do we get an error on invalid SHA1?} {
        catch {r evalsha NotValidShaSUM 0} e
        set _ $e
    } {NOSCRIPT*}

    test {EVALSHA - Do we get an error on non defined SHA1?} {
        catch {r evalsha ffd632c7d33e571e9f24556ebed26c3479a87130 0} e
        set _ $e
    } {NOSCRIPT*}

    test {EVAL - Redis integer -> Lua type conversion} {
        r set x 0
        r eval {
            local foo = redis.pcall('incr',KEYS[1])
            return {type(foo),foo}
        } 1 x
    } {number 1}

    test {EVAL - Redis bulk -> Lua type conversion} {
        r set mykey myval
        r eval {
            local foo = redis.pcall('get',KEYS[1])
            return {type(foo),foo}
        } 1 mykey
    } {string myval}

    test {EVAL - Redis multi bulk -> Lua type conversion} {
        r del mylist
        r rpush mylist a
        r rpush mylist b
        r rpush mylist c
        r eval {
            local foo = redis.pcall('lrange',KEYS[1],0,-1)
            return {type(foo),foo[1],foo[2],foo[3],# foo}
        } 1 mylist
    } {table a b c 3}

    test {EVAL - Redis status reply -> Lua type conversion} {
        r eval {
            local foo = redis.pcall('set',KEYS[1],'myval')
            return {type(foo),foo['ok']}
        } 1 mykey
    } {table OK}

    test {EVAL - Redis error reply -> Lua type conversion} {
        r set mykey myval
        r eval {
            local foo = redis.pcall('incr',KEYS[1])
            return {type(foo),foo['err']}
        } 1 mykey
    } {table {ERR value is not an integer or out of range}}

    test {EVAL - Redis nil bulk reply -> Lua type conversion} {
        r del mykey
        r eval {
            local foo = redis.pcall('get',KEYS[1])
            return {type(foo),foo == false}
        } 1 mykey
    } {boolean 1}

    test {EVAL - Is the Lua client using the currently selected DB?} {
        r set mykey "this is DB 9"
        r select 10
        r set mykey "this is DB 10"
        r eval {return redis.pcall('get',KEYS[1])} 1 mykey
    } {this is DB 10} {singledb:skip}

    test {EVAL - SELECT inside Lua should not affect the caller} {
        # here we DB 10 is selected
        r set mykey "original value"
        r eval {return redis.pcall('select','9')} 0
        set res [r get mykey]
        r select 9
        set res
    } {original value} {singledb:skip}

    if 0 {
        test {EVAL - Script can't run more than configured time limit} {
            r config set lua-time-limit 1
            catch {
                r eval {
                    local i = 0
                    while true do i=i+1 end
                } 0
            } e
            set _ $e
        } {*execution time*}
    }

    test {EVAL - Scripts can't run blpop command} {
        set e {}
        catch {r eval {return redis.pcall('blpop','x',0)} 0} e
        set e
    } {*not allowed*}

    test {EVAL - Scripts can't run brpop command} {
        set e {}
        catch {r eval {return redis.pcall('brpop','empty_list',0)} 0} e
        set e
    } {*not allowed*}

    test {EVAL - Scripts can't run brpoplpush command} {
        set e {}
        catch {r eval {return redis.pcall('brpoplpush','empty_list1', 'empty_list2',0)} 0} e
        set e
    } {*not allowed*}

    test {EVAL - Scripts can't run blmove command} {
        set e {}
        catch {r eval {return redis.pcall('blmove','empty_list1', 'empty_list2', 'LEFT', 'LEFT', 0)} 0} e
        set e
    } {*not allowed*}

    test {EVAL - Scripts can't run bzpopmin command} {
        set e {}
        catch {r eval {return redis.pcall('bzpopmin','empty_zset', 0)} 0} e
        set e
    } {*not allowed*}

    test {EVAL - Scripts can't run bzpopmax command} {
        set e {}
        catch {r eval {return redis.pcall('bzpopmax','empty_zset', 0)} 0} e
        set e
    } {*not allowed*}

    test {EVAL - Scripts can't run XREAD and XREADGROUP with BLOCK option} {
        r del s
        r xgroup create s g $ MKSTREAM
        set res [r eval {return redis.pcall('xread','STREAMS','s','$')} 1 s]
        assert {$res eq {}}
        assert_error "*xread command is not allowed with BLOCK option from scripts" {r eval {return redis.pcall('xread','BLOCK',0,'STREAMS','s','$')} 1 s}
        set res [r eval {return redis.pcall('xreadgroup','group','g','c','STREAMS','s','>')} 1 s]
        assert {$res eq {}}
        assert_error "*xreadgroup command is not allowed with BLOCK option from scripts" {r eval {return redis.pcall('xreadgroup','group','g','c','BLOCK',0,'STREAMS','s','>')} 1 s}
    }

    test {EVAL - Scripts can't run certain commands} {
        set e {}
        r debug lua-always-replicate-commands 0
        catch {
            r eval "redis.pcall('randomkey'); return redis.pcall('set','x','ciao')" 0
        } e
        r debug lua-always-replicate-commands 1
        set e
    } {*not allowed after*} {needs:debug}

    test {EVAL - No arguments to redis.call/pcall is considered an error} {
        set e {}
        catch {r eval {return redis.call()} 0} e
        set e
    } {*one argument*}

    test {EVAL - redis.call variant raises a Lua error on Redis cmd error (1)} {
        set e {}
        catch {
            r eval "redis.call('nosuchcommand')" 0
        } e
        set e
    } {*Unknown Redis*}

    test {EVAL - redis.call variant raises a Lua error on Redis cmd error (1)} {
        set e {}
        catch {
            r eval "redis.call('get','a','b','c')" 0
        } e
        set e
    } {*number of args*}

    test {EVAL - redis.call variant raises a Lua error on Redis cmd error (1)} {
        set e {}
        r set foo bar
        catch {
            r eval {redis.call('lpush',KEYS[1],'val')} 1 foo
        } e
        set e
    } {*against a key*}

    test {EVAL - JSON numeric decoding} {
        # We must return the table as a string because otherwise
        # Redis converts floats to ints and we get 0 and 1023 instead
        # of 0.0003 and 1023.2 as the parsed output.
        r eval {return
                 table.concat(
                   cjson.decode(
                    "[0.0, -5e3, -1, 0.3e-3, 1023.2, 0e10]"), " ")
        } 0
    } {0 -5000 -1 0.0003 1023.2 0}

    test {EVAL - JSON string decoding} {
        r eval {local decoded = cjson.decode('{"keya": "a", "keyb": "b"}')
                return {decoded.keya, decoded.keyb}
        } 0
    } {a b}

    test {EVAL - cmsgpack can pack double?} {
        r eval {local encoded = cmsgpack.pack(0.1)
                local h = ""
                for i = 1, #encoded do
                    h = h .. string.format("%02x",string.byte(encoded,i))
                end
                return h
        } 0
    } {cb3fb999999999999a}

    test {EVAL - cmsgpack can pack negative int64?} {
        r eval {local encoded = cmsgpack.pack(-1099511627776)
                local h = ""
                for i = 1, #encoded do
                    h = h .. string.format("%02x",string.byte(encoded,i))
                end
                return h
        } 0
    } {d3ffffff0000000000}

    test {EVAL - cmsgpack can pack and unpack circular references?} {
        r eval {local a = {x=nil,y=5}
                local b = {x=a}
                a['x'] = b
                local encoded = cmsgpack.pack(a)
                local h = ""
                -- cmsgpack encodes to a depth of 16, but can't encode
                -- references, so the encoded object has a deep copy recursive
                -- depth of 16.
                for i = 1, #encoded do
                    h = h .. string.format("%02x",string.byte(encoded,i))
                end
                -- when unpacked, re.x.x != re because the unpack creates
                -- individual tables down to a depth of 16.
                -- (that's why the encoded output is so large)
                local re = cmsgpack.unpack(encoded)
                assert(re)
                assert(re.x)
                assert(re.x.x.y == re.y)
                assert(re.x.x.x.x.y == re.y)
                assert(re.x.x.x.x.x.x.y == re.y)
                assert(re.x.x.x.x.x.x.x.x.x.x.y == re.y)
                -- maximum working depth:
                assert(re.x.x.x.x.x.x.x.x.x.x.x.x.x.x.y == re.y)
                -- now the last x would be b above and has no y
                assert(re.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x)
                -- so, the final x.x is at the depth limit and was assigned nil
                assert(re.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x.x == nil)
                return {h, re.x.x.x.x.x.x.x.x.y == re.y, re.y == 5}
        } 0
    } {82a17905a17881a17882a17905a17881a17882a17905a17881a17882a17905a17881a17882a17905a17881a17882a17905a17881a17882a17905a17881a17882a17905a17881a178c0 1 1}

    test {EVAL - Numerical sanity check from bitop} {
        r eval {assert(0x7fffffff == 2147483647, "broken hex literals");
                assert(0xffffffff == -1 or 0xffffffff == 2^32-1,
                    "broken hex literals");
                assert(tostring(-1) == "-1", "broken tostring()");
                assert(tostring(0xffffffff) == "-1" or
                    tostring(0xffffffff) == "4294967295",
                    "broken tostring()")
        } 0
    } {}

    test {EVAL - Verify minimal bitop functionality} {
        r eval {assert(bit.tobit(1) == 1);
                assert(bit.band(1) == 1);
                assert(bit.bxor(1,2) == 3);
                assert(bit.bor(1,2,4,8,16,32,64,128) == 255)
        } 0
    } {}

    test {EVAL - Able to parse trailing comments} {
        r eval {return 'hello' --trailing comment} 0
    } {hello}

    test {EVAL_RO - Successful case} {
        r set foo bar
        assert_equal bar [r eval_ro {return redis.call('get', KEYS[1]);} 1 foo]
    }

    test {EVAL_RO - Cannot run write commands} {
        r set foo bar
        catch {r eval_ro {redis.call('del', KEYS[1]);} 1 foo} e
        set e
    } {*Write commands are not allowed from read-only scripts*}

    test {SCRIPTING FLUSH - is able to clear the scripts cache?} {
        r set mykey myval
        set v [r evalsha fd758d1589d044dd850a6f05d52f2eefd27f033f 1 mykey]
        assert_equal $v myval
        set e ""
        r script flush
        catch {r evalsha fd758d1589d044dd850a6f05d52f2eefd27f033f 1 mykey} e
        set e
    } {NOSCRIPT*}

    test {SCRIPTING FLUSH ASYNC} {
        for {set j 0} {$j < 100} {incr j} {
            r script load "return $j"
        }
        assert { [string match "*number_of_cached_scripts:100*" [r info Memory]] }
        r script flush async
        assert { [string match "*number_of_cached_scripts:0*" [r info Memory]] }
    }

    test {SCRIPT EXISTS - can detect already defined scripts?} {
        r eval "return 1+1" 0
        r script exists a27e7e8a43702b7046d4f6a7ccf5b60cef6b9bd9 a27e7e8a43702b7046d4f6a7ccf5b60cef6b9bda
    } {1 0}

    test {SCRIPT LOAD - is able to register scripts in the scripting cache} {
        list \
            [r script load "return 'loaded'"] \
            [r evalsha b534286061d4b9e4026607613b95c06c06015ae8 0]
    } {b534286061d4b9e4026607613b95c06c06015ae8 loaded}

    test "In the context of Lua the output of random commands gets ordered" {
        r debug lua-always-replicate-commands 0
        r del myset
        r sadd myset a b c d e f g h i l m n o p q r s t u v z aa aaa azz
        set res [r eval {return redis.call('smembers',KEYS[1])} 1 myset]
        r debug lua-always-replicate-commands 1
        set res
    } {a aa aaa azz b c d e f g h i l m n o p q r s t u v z} {needs:debug}

    test "SORT is normally not alpha re-ordered for the scripting engine" {
        r del myset
        r sadd myset 1 2 3 4 10
        r eval {return redis.call('sort',KEYS[1],'desc')} 1 myset
    } {10 4 3 2 1} {cluster:skip}

    test "SORT BY <constant> output gets ordered for scripting" {
        r del myset
        r sadd myset a b c d e f g h i l m n o p q r s t u v z aa aaa azz
        r eval {return redis.call('sort',KEYS[1],'by','_')} 1 myset
    } {a aa aaa azz b c d e f g h i l m n o p q r s t u v z} {cluster:skip}

    test "SORT BY <constant> with GET gets ordered for scripting" {
        r del myset
        r sadd myset a b c
        r eval {return redis.call('sort',KEYS[1],'by','_','get','#','get','_:*')} 1 myset
    } {a {} b {} c {}} {cluster:skip}

    test "redis.sha1hex() implementation" {
        list [r eval {return redis.sha1hex('')} 0] \
             [r eval {return redis.sha1hex('Pizza & Mandolino')} 0]
    } {da39a3ee5e6b4b0d3255bfef95601890afd80709 74822d82031af7493c20eefa13bd07ec4fada82f}

    test {Globals protection reading an undeclared global variable} {
        catch {r eval {return a} 0} e
        set e
    } {*ERR*attempted to access * global*}

    test {Globals protection setting an undeclared global*} {
        catch {r eval {a=10} 0} e
        set e
    } {*ERR*attempted to create global*}

    test {Test an example script DECR_IF_GT} {
        set decr_if_gt {
            local current

            current = redis.call('get',KEYS[1])
            if not current then return nil end
            if current > ARGV[1] then
                return redis.call('decr',KEYS[1])
            else
                return redis.call('get',KEYS[1])
            end
        }
        r set foo 5
        set res {}
        lappend res [r eval $decr_if_gt 1 foo 2]
        lappend res [r eval $decr_if_gt 1 foo 2]
        lappend res [r eval $decr_if_gt 1 foo 2]
        lappend res [r eval $decr_if_gt 1 foo 2]
        lappend res [r eval $decr_if_gt 1 foo 2]
        set res
    } {4 3 2 2 2}

    test {Scripting engine resets PRNG at every script execution} {
        set rand1 [r eval {return tostring(math.random())} 0]
        set rand2 [r eval {return tostring(math.random())} 0]
        assert_equal $rand1 $rand2
    }

    test {Scripting engine PRNG can be seeded correctly} {
        set rand1 [r eval {
            math.randomseed(ARGV[1]); return tostring(math.random())
        } 0 10]
        set rand2 [r eval {
            math.randomseed(ARGV[1]); return tostring(math.random())
        } 0 10]
        set rand3 [r eval {
            math.randomseed(ARGV[1]); return tostring(math.random())
        } 0 20]
        assert_equal $rand1 $rand2
        assert {$rand2 ne $rand3}
    }

    test {EVAL does not leak in the Lua stack} {
        r set x 0
        # Use a non blocking client to speedup the loop.
        set rd [redis_deferring_client]
        for {set j 0} {$j < 10000} {incr j} {
            $rd eval {return redis.call("incr",KEYS[1])} 1 x
        }
        for {set j 0} {$j < 10000} {incr j} {
            $rd read
        }
        assert {[s used_memory_lua] < 1024*100}
        $rd close
        r get x
    } {10000}

    test {EVAL processes writes from AOF in read-only slaves} {
        r flushall
        r config set appendonly yes
        r config set aof-use-rdb-preamble no
        r eval {redis.call("set",KEYS[1],"100")} 1 foo
        r eval {redis.call("incr",KEYS[1])} 1 foo
        r eval {redis.call("incr",KEYS[1])} 1 foo
        wait_for_condition 50 100 {
            [s aof_rewrite_in_progress] == 0
        } else {
            fail "AOF rewrite can't complete after CONFIG SET appendonly yes."
        }
        r config set slave-read-only yes
        r slaveof 127.0.0.1 0
        r debug loadaof
        set res [r get foo]
        r slaveof no one
        r config set aof-use-rdb-preamble yes
        set res
    } {102} {external:skip}

    test {EVAL timeout from AOF} {
        # generate a long running script that is propagated to the AOF as script
        # make sure that the script times out during loading
        r config set appendonly no
        r config set aof-use-rdb-preamble no
        r config set lua-replicate-commands no
        r flushall
        r config set appendonly yes
        wait_for_condition 50 100 {
            [s aof_rewrite_in_progress] == 0
        } else {
            fail "AOF rewrite can't complete after CONFIG SET appendonly yes."
        }
        r config set lua-time-limit 1
        set rd [redis_deferring_client]
        set start [clock clicks -milliseconds]
        $rd eval {redis.call('set',KEYS[1],'y'); for i=1,1500000 do redis.call('ping') end return 'ok'} 1 x
        $rd flush
        after 100
        catch {r ping} err
        assert_match {BUSY*} $err
        $rd read
        set elapsed [expr [clock clicks -milliseconds]-$start]
        if {$::verbose} { puts "script took $elapsed milliseconds" }
        set start [clock clicks -milliseconds]
        $rd debug loadaof
        $rd flush
        after 100
        catch {r ping} err
        assert_match {LOADING*} $err
        $rd read
        set elapsed [expr [clock clicks -milliseconds]-$start]
        if {$::verbose} { puts "loading took $elapsed milliseconds" }
        $rd close
        r get x
    } {y} {external:skip}

    test {We can call scripts rewriting client->argv from Lua} {
        r del myset
        r sadd myset a b c
        r mset a{t} 1 b{t} 2 c{t} 3 d{t} 4
        assert {[r spop myset] ne {}}
        assert {[r spop myset 1] ne {}}
        assert {[r spop myset] ne {}}
        assert {[r mget a{t} b{t} c{t} d{t}] eq {1 2 3 4}}
        assert {[r spop myset] eq {}}
    }

    test {Call Redis command with many args from Lua (issue #1764)} {
        r eval {
            local i
            local x={}
            redis.call('del','mylist')
            for i=1,100 do
                table.insert(x,i)
            end
            redis.call('rpush','mylist',unpack(x))
            return redis.call('lrange','mylist',0,-1)
        } 1 mylist
    } {1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20 21 22 23 24 25 26 27 28 29 30 31 32 33 34 35 36 37 38 39 40 41 42 43 44 45 46 47 48 49 50 51 52 53 54 55 56 57 58 59 60 61 62 63 64 65 66 67 68 69 70 71 72 73 74 75 76 77 78 79 80 81 82 83 84 85 86 87 88 89 90 91 92 93 94 95 96 97 98 99 100}

    test {Number conversion precision test (issue #1118)} {
        r eval {
              local value = 9007199254740991
              redis.call("set","foo",value)
              return redis.call("get","foo")
        } 1 foo
    } {9007199254740991}

    test {String containing number precision test (regression of issue #1118)} {
        r eval {
            redis.call("set", "key", "12039611435714932082")
            return redis.call("get", "key")
        } 1 key
    } {12039611435714932082}

    test {Verify negative arg count is error instead of crash (issue #1842)} {
        catch { r eval { return "hello" } -12 } e
        set e
    } {ERR Number of keys can't be negative}

    test {Correct handling of reused argv (issue #1939)} {
        r eval {
              for i = 0, 10 do
                  redis.call('SET', 'a{t}', '1')
                  redis.call('MGET', 'a{t}', 'b{t}', 'c{t}')
                  redis.call('EXPIRE', 'a{t}', 0)
                  redis.call('GET', 'a{t}')
                  redis.call('MGET', 'a{t}', 'b{t}', 'c{t}')
              end
        } 3 a{t} b{t} c{t}
    }

    test {Functions in the Redis namespace are able to report errors} {
        catch {
            r eval {
                  redis.sha1hex()
            } 0
        } e
        set e
    } {*wrong number*}

    test {Script with RESP3 map} {
        set expected_dict [dict create field value]
        set expected_list [list field value]

        # Sanity test for RESP3 without scripts
        r HELLO 3
        r hset hash field value
        set res [r hgetall hash]
        assert_equal $res $expected_dict

        # Test RESP3 client with script in both RESP2 and RESP3 modes
        set res [r eval {redis.setresp(3); return redis.call('hgetall', KEYS[1])} 1 hash]
        assert_equal $res $expected_dict
        set res [r eval {redis.setresp(2); return redis.call('hgetall', KEYS[1])} 1 hash]
        assert_equal $res $expected_list

        # Test RESP2 client with script in both RESP2 and RESP3 modes
        r HELLO 2
        set res [r eval {redis.setresp(3); return redis.call('hgetall', KEYS[1])} 1 hash]
        assert_equal $res $expected_list
        set res [r eval {redis.setresp(2); return redis.call('hgetall', KEYS[1])} 1 hash]
        assert_equal $res $expected_list
    }
}

# Start a new server since the last test in this stanza will kill the
# instance at all.
start_server {tags {"scripting"}} {
    test {Timedout read-only scripts can be killed by SCRIPT KILL} {
        set rd [redis_deferring_client]
        r config set lua-time-limit 10
        $rd eval {while true do end} 0
        after 200
        catch {r ping} e
        assert_match {BUSY*} $e
        r script kill
        after 200 ; # Give some time to Lua to call the hook again...
        assert_equal [r ping] "PONG"
    }

    test {Timedout read-only scripts can be killed by SCRIPT KILL even when use pcall} {
        set rd [redis_deferring_client]
        r config set lua-time-limit 10
        $rd eval {local f = function() while 1 do redis.call('ping') end end while 1 do pcall(f) end} 0
        
        wait_for_condition 50 100 {
            [catch {r ping} e] == 1
        } else {
            fail "Can't wait for script to start running"
        }
        catch {r ping} e
        assert_match {BUSY*} $e

        r script kill

        wait_for_condition 50 100 {
            [catch {r ping} e] == 0
        } else {
            fail "Can't wait for script to be killed"
        }
        assert_equal [r ping] "PONG"

        catch {$rd read} res
        $rd close

        assert_match {*killed by user*} $res        
    }

    test {Timedout script does not cause a false dead client} {
        set rd [redis_deferring_client]
        r config set lua-time-limit 10

        # senging (in a pipeline):
        # 1. eval "while 1 do redis.call('ping') end" 0
        # 2. ping
        set buf "*3\r\n\$4\r\neval\r\n\$33\r\nwhile 1 do redis.call('ping') end\r\n\$1\r\n0\r\n"
        append buf "*1\r\n\$4\r\nping\r\n"
        $rd write $buf
        $rd flush

        wait_for_condition 50 100 {
            [catch {r ping} e] == 1
        } else {
            fail "Can't wait for script to start running"
        }
        catch {r ping} e
        assert_match {BUSY*} $e

        r script kill
        wait_for_condition 50 100 {
            [catch {r ping} e] == 0
        } else {
            fail "Can't wait for script to be killed"
        }
        assert_equal [r ping] "PONG"

        catch {$rd read} res
        assert_match {*killed by user*} $res

        set res [$rd read]
        assert_match {*PONG*} $res        

        $rd close
    }

    test {Timedout script link is still usable after Lua returns} {
        r config set lua-time-limit 10
        r eval {for i=1,100000 do redis.call('ping') end return 'ok'} 0
        r ping
    } {PONG}

    test {Timedout scripts that modified data can't be killed by SCRIPT KILL} {
        set rd [redis_deferring_client]
        r config set lua-time-limit 10
        $rd eval {redis.call('set',KEYS[1],'y'); while true do end} 1 x
        after 200
        catch {r ping} e
        assert_match {BUSY*} $e
        catch {r script kill} e
        assert_match {UNKILLABLE*} $e
        catch {r ping} e
        assert_match {BUSY*} $e
    } {} {external:skip}

    # Note: keep this test at the end of this server stanza because it
    # kills the server.
    test {SHUTDOWN NOSAVE can kill a timedout script anyway} {
        # The server should be still unresponding to normal commands.
        catch {r ping} e
        assert_match {BUSY*} $e
        catch {r shutdown nosave}
        # Make sure the server was killed
        catch {set rd [redis_deferring_client]} e
        assert_match {*connection refused*} $e
    } {} {external:skip}
}

foreach cmdrepl {0 1} {
    start_server {tags {"scripting repl needs:debug external:skip"}} {
        start_server {} {
            if {$cmdrepl == 1} {
                set rt "(commands replication)"
            } else {
                set rt "(scripts replication)"
                r debug lua-always-replicate-commands 1
            }

            test "Before the replica connects we issue two EVAL commands $rt" {
                # One with an error, but still executing a command.
                # SHA is: 67164fc43fa971f76fd1aaeeaf60c1c178d25876
                catch {
                    r eval {redis.call('incr',KEYS[1]); redis.call('nonexisting')} 1 x
                }
                # One command is correct:
                # SHA is: 6f5ade10a69975e903c6d07b10ea44c6382381a5
                r eval {return redis.call('incr',KEYS[1])} 1 x
            } {2}

            test "Connect a replica to the master instance $rt" {
                r -1 slaveof [srv 0 host] [srv 0 port]
                wait_for_condition 50 100 {
                    [s -1 role] eq {slave} &&
                    [string match {*master_link_status:up*} [r -1 info replication]]
                } else {
                    fail "Can't turn the instance into a replica"
                }
            }

            test "Now use EVALSHA against the master, with both SHAs $rt" {
                # The server should replicate successful and unsuccessful
                # commands as EVAL instead of EVALSHA.
                catch {
                    r evalsha 67164fc43fa971f76fd1aaeeaf60c1c178d25876 1 x
                }
                r evalsha 6f5ade10a69975e903c6d07b10ea44c6382381a5 1 x
            } {4}

            test "If EVALSHA was replicated as EVAL, 'x' should be '4' $rt" {
                wait_for_condition 50 100 {
                    [r -1 get x] eq {4}
                } else {
                    fail "Expected 4 in x, but value is '[r -1 get x]'"
                }
            }

            test "Replication of script multiple pushes to list with BLPOP $rt" {
                set rd [redis_deferring_client]
                $rd brpop a 0
                r eval {
                    redis.call("lpush",KEYS[1],"1");
                    redis.call("lpush",KEYS[1],"2");
                } 1 a
                set res [$rd read]
                $rd close
                wait_for_condition 50 100 {
                    [r -1 lrange a 0 -1] eq [r lrange a 0 -1]
                } else {
                    fail "Expected list 'a' in replica and master to be the same, but they are respectively '[r -1 lrange a 0 -1]' and '[r lrange a 0 -1]'"
                }
                set res
            } {a 1}

            test "EVALSHA replication when first call is readonly $rt" {
                r del x
                r eval {if tonumber(ARGV[1]) > 0 then redis.call('incr', KEYS[1]) end} 1 x 0
                r evalsha 6e0e2745aa546d0b50b801a20983b70710aef3ce 1 x 0
                r evalsha 6e0e2745aa546d0b50b801a20983b70710aef3ce 1 x 1
                wait_for_condition 50 100 {
                    [r -1 get x] eq {1}
                } else {
                    fail "Expected 1 in x, but value is '[r -1 get x]'"
                }
            }

            test "Lua scripts using SELECT are replicated correctly $rt" {
                r eval {
                    redis.call("set","foo1","bar1")
                    redis.call("select","10")
                    redis.call("incr","x")
                    redis.call("select","11")
                    redis.call("incr","z")
                } 0
                r eval {
                    redis.call("set","foo1","bar1")
                    redis.call("select","10")
                    redis.call("incr","x")
                    redis.call("select","11")
                    redis.call("incr","z")
                } 0
                wait_for_condition 50 100 {
                    [r -1 debug digest] eq [r debug digest]
                } else {
                    fail "Master-Replica desync after Lua script using SELECT."
                }
            } {} {singledb:skip}
        }
    }
}

start_server {tags {"scripting repl external:skip"}} {
    start_server {overrides {appendonly yes aof-use-rdb-preamble no}} {
        test "Connect a replica to the master instance" {
            r -1 slaveof [srv 0 host] [srv 0 port]
            wait_for_condition 50 100 {
                [s -1 role] eq {slave} &&
                [string match {*master_link_status:up*} [r -1 info replication]]
            } else {
                fail "Can't turn the instance into a replica"
            }
        }

        test "Redis.replicate_commands() must be issued before any write" {
            r eval {
                redis.call('set','foo','bar');
                return redis.replicate_commands();
            } 0
        } {}

        test "Redis.replicate_commands() must be issued before any write (2)" {
            r eval {
                return redis.replicate_commands();
            } 0
        } {1}

        test "Redis.set_repl() must be issued after replicate_commands()" {
            r debug lua-always-replicate-commands 0
            catch {
                r eval {
                    redis.set_repl(redis.REPL_ALL);
                } 0
            } e
            r debug lua-always-replicate-commands 1
            set e
        } {*only after turning on*}

        test "Redis.set_repl() don't accept invalid values" {
            catch {
                r eval {
                    redis.replicate_commands();
                    redis.set_repl(12345);
                } 0
            } e
            set e
        } {*Invalid*flags*}

        test "Test selective replication of certain Redis commands from Lua" {
            r del a b c d
            r eval {
                redis.replicate_commands();
                redis.call('set','a','1');
                redis.set_repl(redis.REPL_NONE);
                redis.call('set','b','2');
                redis.set_repl(redis.REPL_AOF);
                redis.call('set','c','3');
                redis.set_repl(redis.REPL_ALL);
                redis.call('set','d','4');
            } 0

            wait_for_condition 50 100 {
                [r -1 mget a b c d] eq {1 {} {} 4}
            } else {
                fail "Only a and c should be replicated to replica"
            }

            # Master should have everything right now
            assert {[r mget a b c d] eq {1 2 3 4}}

            # After an AOF reload only a, c and d should exist
            r debug loadaof

            assert {[r mget a b c d] eq {1 {} 3 4}}
        }

        test "PRNG is seeded randomly for command replication" {
            set a [
                r eval {
                    redis.replicate_commands();
                    return math.random()*100000;
                } 0
            ]
            set b [
                r eval {
                    redis.replicate_commands();
                    return math.random()*100000;
                } 0
            ]
            assert {$a ne $b}
        }

        test "Using side effects is not a problem with command replication" {
            r eval {
                redis.replicate_commands();
                redis.call('set','time',redis.call('time')[1])
            } 0

            assert {[r get time] ne {}}

            wait_for_condition 50 100 {
                [r get time] eq [r -1 get time]
            } else {
                fail "Time key does not match between master and replica"
            }
        }
    }
}

start_server {tags {"scripting external:skip"}} {
    r script debug sync
    r eval {return 'hello'} 0
    r eval {return 'hello'} 0
}

start_server {tags {"scripting resp3 needs:debug"}} {
    r debug set-disable-deny-scripts 1
    for {set i 2} {$i <= 3} {incr i} {
        for {set client_proto 2} {$client_proto <= 3} {incr client_proto} {
            r hello $client_proto
            r readraw 1

            test {test resp3 big number protocol parsing} {
                set ret [r eval "redis.setresp($i);return redis.call('debug', 'protocol', 'bignum')" 0]
                if {$client_proto == 2 || $i == 2} {
                    # if either Lua or the clien is RESP2 the reply will be RESP2
                    assert_equal $ret {$37}
                    assert_equal [r read] {1234567999999999999999999999999999999}
                } else {
                    assert_equal $ret {(1234567999999999999999999999999999999}
                }
            }

            test {test resp3 map protocol parsing} {
                set ret [r eval "redis.setresp($i);return redis.call('debug', 'protocol', 'map')" 0]
                if {$client_proto == 2 || $i == 2} {
                    # if either Lua or the clien is RESP2 the reply will be RESP2
                    assert_equal $ret {*6}
                } else {
                    assert_equal $ret {%3}
                }
                for {set j 0} {$j < 6} {incr j} {
                    r read
                }
            }

            test {test resp3 set protocol parsing} {
                set ret [r eval "redis.setresp($i);return redis.call('debug', 'protocol', 'set')" 0]
                if {$client_proto == 2 || $i == 2} {
                    # if either Lua or the clien is RESP2 the reply will be RESP2
                    assert_equal $ret {*3}
                } else {
                    assert_equal $ret {~3}
                }
                for {set j 0} {$j < 3} {incr j} {
                    r read
                }
            }

            test {test resp3 double protocol parsing} {
                set ret [r eval "redis.setresp($i);return redis.call('debug', 'protocol', 'double')" 0]
                if {$client_proto == 2 || $i == 2} {
                    # if either Lua or the clien is RESP2 the reply will be RESP2
                    assert_equal $ret {$18}
                    assert_equal [r read] {3.1415926535900001}
                } else {
                    assert_equal $ret {,3.1415926535900001}
                }
            }

            test {test resp3 null protocol parsing} {
                set ret [r eval "redis.setresp($i);return redis.call('debug', 'protocol', 'null')" 0]
                if {$client_proto == 2} {
                    # null is a special case in which a Lua client format does not effect the reply to the client
                    assert_equal $ret {$-1}
                } else {
                    assert_equal $ret {_}
                }
            } {}

            test {test resp3 verbatim protocol parsing} {
                set ret [r eval "redis.setresp($i);return redis.call('debug', 'protocol', 'verbatim')" 0]
                if {$client_proto == 2 || $i == 2} {
                    # if either Lua or the clien is RESP2 the reply will be RESP2
                    assert_equal $ret {$25}
                    assert_equal [r read] {This is a verbatim}
                    assert_equal [r read] {string}
                } else {
                    assert_equal $ret {=29}
                    assert_equal [r read] {txt:This is a verbatim}
                    assert_equal [r read] {string}
                }   
            }

            test {test resp3 true protocol parsing} {
                set ret [r eval "redis.setresp($i);return redis.call('debug', 'protocol', 'true')" 0]
                if {$client_proto == 2 || $i == 2} {
                    # if either Lua or the clien is RESP2 the reply will be RESP2
                    assert_equal $ret {:1}
                } else {
                    assert_equal $ret {#t}
                }
            }

            test {test resp3 false protocol parsing} {
                set ret [r eval "redis.setresp($i);return redis.call('debug', 'protocol', 'false')" 0]
                if {$client_proto == 2 || $i == 2} {
                    # if either Lua or the clien is RESP2 the reply will be RESP2
                    assert_equal $ret {:0}
                } else {
                    assert_equal $ret {#f}
                }
            }

            r readraw 0
        }
    }

    # attribute is not relevant to test with resp2
    test {test resp3 attribute protocol parsing} {
        # attributes are not (yet) expose to the script
        # So here we just check the parser handles them and they are ignored.
        r eval "redis.setresp(3);return redis.call('debug', 'protocol', 'attrib')" 0
    } {Some real reply following the attribute}

    r debug set-disable-deny-scripts 0
}