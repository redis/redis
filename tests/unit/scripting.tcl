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

    test {EVAL - Are the KEYS and ARGS arrays populated correctly?} {
        r eval {return {KEYS[1],KEYS[2],ARGV[1],ARGV[2]}} 2 a b c d
    } {a b c d}

    test {EVAL - is Lua able to call Redis API?} {
        r set mykey myval
        r eval {return redis.call('get','mykey')} 0
    } {myval}

    test {EVALSHA - Can we call a SHA1 if already defined?} {
        r evalsha 9bd632c7d33e571e9f24556ebed26c3479a87129 0
    } {myval}

    test {EVALSHA - Can we call a SHA1 in uppercase?} {
        r evalsha 9BD632C7D33E571E9F24556EBED26C3479A87129 0
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
        r eval {
            local foo = redis.pcall('incr','x')
            return {type(foo),foo}
        } 0
    } {number 1}

    test {EVAL - Redis bulk -> Lua type conversion} {
        r set mykey myval
        r eval {
            local foo = redis.pcall('get','mykey')
            return {type(foo),foo}
        } 0
    } {string myval}

    test {EVAL - Redis multi bulk -> Lua type conversion} {
        r del mylist
        r rpush mylist a
        r rpush mylist b
        r rpush mylist c
        r eval {
            local foo = redis.pcall('lrange','mylist',0,-1)
            return {type(foo),foo[1],foo[2],foo[3],# foo}
        } 0
    } {table a b c 3}

    test {EVAL - Redis status reply -> Lua type conversion} {
        r eval {
            local foo = redis.pcall('set','mykey','myval')
            return {type(foo),foo['ok']}
        } 0
    } {table OK}

    test {EVAL - Redis error reply -> Lua type conversion} {
        r set mykey myval
        r eval {
            local foo = redis.pcall('incr','mykey')
            return {type(foo),foo['err']}
        } 0
    } {table {ERR value is not an integer or out of range}}

    test {EVAL - Redis nil bulk reply -> Lua type conversion} {
        r del mykey
        r eval {
            local foo = redis.pcall('get','mykey')
            return {type(foo),foo == false}
        } 0
    } {boolean 1}

    test {EVAL - Is Lua affecting the currently selected DB?} {
        r set mykey "this is DB 9"
        r select 10
        r set mykey "this is DB 10"
        r eval {return redis.pcall('get','mykey')} 0
    } {this is DB 10}

    test {EVAL - Is Lua seleced DB retained?} {
        r eval {return redis.pcall('select','9')} 0
        r get mykey
    } {this is DB 9}

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

    test {EVAL - Scripts can't run certain commands} {
        set e {}
        catch {r eval {return redis.pcall('spop','x')} 0} e
        set e
    } {*not allowed*}

    test {EVAL - Scripts can't run certain commands} {
        set e {}
        catch {
            r eval "redis.pcall('randomkey'); return redis.pcall('set','x','ciao')" 0
        } e
        set e
    } {*not allowed after*}

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
            r eval "redis.call('lpush','foo','val')" 0
        } e
        set e
    } {*against a key*}

    test {SCRIPTING FLUSH - is able to clear the scripts cache?} {
        r set mykey myval
        set v [r evalsha 9bd632c7d33e571e9f24556ebed26c3479a87129 0]
        assert_equal $v myval
        set e ""
        r script flush
        catch {r evalsha 9bd632c7d33e571e9f24556ebed26c3479a87129 0} e
        set e
    } {NOSCRIPT*}

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
        r del myset
        r sadd myset a b c d e f g h i l m n o p q r s t u v z aa aaa azz
        r eval {return redis.call('smembers','myset')} 0
    } {a aa aaa azz b c d e f g h i l m n o p q r s t u v z}

    test "SORT is normally not alpha re-ordered for the scripting engine" {
        r del myset
        r sadd myset 1 2 3 4 10
        r eval {return redis.call('sort','myset','desc')} 0
    } {10 4 3 2 1}

    test "SORT BY <constant> output gets ordered for scripting" {
        r del myset
        r sadd myset a b c d e f g h i l m n o p q r s t u v z aa aaa azz
        r eval {return redis.call('sort','myset','by','_')} 0
    } {a aa aaa azz b c d e f g h i l m n o p q r s t u v z}

    test "SORT BY <constant> with GET gets ordered for scripting" {
        r del myset
        r sadd myset a b c
        r eval {return redis.call('sort','myset','by','_','get','#','get','_:*')} 0
    } {a {} b {} c {}}

    test "redis.sha1hex() implementation" {
        list [r eval {return redis.sha1hex('')} 0] \
             [r eval {return redis.sha1hex('Pizza & Mandolino')} 0]
    } {da39a3ee5e6b4b0d3255bfef95601890afd80709 74822d82031af7493c20eefa13bd07ec4fada82f}

    test {Globals protection reading an undeclared global variable} {
        catch {r eval {return a} 0} e
        set e
    } {*ERR*attempted to access unexisting global*}

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
        assert_equal [r ping] "PONG"
    }

    test {Timedout script link is still usable after Lua returns} {
        r config set lua-time-limit 10
        r eval {for i=1,100000 do redis.call('ping') end return 'ok'} 0
        r ping
    } {PONG}

    test {Timedout scripts that modified data can't be killed by SCRIPT KILL} {
        set rd [redis_deferring_client]
        r config set lua-time-limit 10
        $rd eval {redis.call('set','x','y'); while true do end} 0
        after 200
        catch {r ping} e
        assert_match {BUSY*} $e
        catch {r script kill} e
        assert_match {UNKILLABLE*} $e
        catch {r ping} e
        assert_match {BUSY*} $e
    }

    # Note: keep this test at the end of this server stanza because it
    # kills the server.
    test {SHUTDOWN NOSAVE can kill a timedout script anyway} {
        # The server sould be still unresponding to normal commands.
        catch {r ping} e
        assert_match {BUSY*} $e
        catch {r shutdown nosave}
        # Make sure the server was killed
        catch {set rd [redis_deferring_client]} e
        assert_match {*connection refused*} $e
    }
}

start_server {tags {"scripting repl"}} {
    start_server {} {
        test {Before the slave connects we issue two EVAL commands} {
            # One with an error, but still executing a command.
            # SHA is: 6e8bd6bdccbe78899e3cc06b31b6dbf4324c2e56
            catch {
                r eval {redis.call('incr','x'); redis.call('nonexisting')} 0
            }
            # One command is correct:
            # SHA is: ae3477e27be955de7e1bc9adfdca626b478d3cb2
            r eval {return redis.call('incr','x')} 0
        } {2}

        test {Connect a slave to the main instance} {
            r -1 slaveof [srv 0 host] [srv 0 port]
            wait_for_condition 50 100 {
                [s -1 role] eq {slave} &&
                [string match {*master_link_status:up*} [r -1 info replication]]
            } else {
                fail "Can't turn the instance into a slave"
            }
        }

        test {Now use EVALSHA against the master, with both SHAs} {
            # The server should replicate successful and unsuccessful
            # commands as EVAL instead of EVALSHA.
            catch {
                r evalsha 6e8bd6bdccbe78899e3cc06b31b6dbf4324c2e56 0
            }
            r evalsha ae3477e27be955de7e1bc9adfdca626b478d3cb2 0
        } {4}

        test {If EVALSHA was replicated as EVAL, 'x' should be '4'} {
            wait_for_condition 50 100 {
                [r -1 get x] eq {4}
            } else {
                fail "Expected 4 in x, but value is '[r -1 get x]'"
            }
        }

        test {Replication of script multiple pushes to list with BLPOP} {
            set rd [redis_deferring_client]
            $rd brpop a 0
            r eval {
                redis.call("lpush","a","1");
                redis.call("lpush","a","2");
            } 0
            set res [$rd read]
            $rd close
            wait_for_condition 50 100 {
                [r -1 lrange a 0 -1] eq [r lrange a 0 -1]
            } else {
                fail "Expected list 'a' in slave and master to be the same, but they are respectively '[r -1 lrange a 0 -1]' and '[r lrange a 0 -1]'"
            }
            set res
        } {a 1}
    }
}

start_server {tags {"persistent script repl"}} {
    start_server {} {
        test {Load read-only script 99 into the main instance using EVAL} {
            r eval {return 99} 0
        } {99}

        test {Connect a slave to the main instance} {
            r -1 slaveof [srv 0 host] [srv 0 port]
            wait_for_condition 50 100 {
                [s -1 role] eq {slave} &&
                [string match {*master_link_status:up*} [r -1 info replication]]
            } else {
                fail "Can't turn the instance into a slave"
            }
        }

        test {EVALSHA against slave to run script 99} {
            r -1 evalsha a2eba4a6e7d6643bc4b14aa4f9e0eedd55dbae3d 0
        } {99}

        test {Load read-only script 98 into the main instance using EVAL} {
            r eval {return 98} 0
        } {98}

        test {EVALSHA against slave to run script 98} {
            r -1 evalsha 069e4d5d0c3afa79c9a545d8c620dbb94c2a5996 0
        } {98}

        test {Load read-only script 97 into the main instance using SCRIPT LOAD} {
            r script load {return 97}
        } {3f1673c4bedd6a9bc81500f26a454cd277092f80}

        test {EVALSHA against slave to run script 97} {
            r -1 evalsha 3f1673c4bedd6a9bc81500f26a454cd277092f80 0
        } {97}
    }
}
