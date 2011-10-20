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

    test {EVALSHA - Do we get an error on non defined SHA1?} {
        catch {r evalsha ffffffffffffffffffffffffffffffffffffffff 0} e
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
}

start_server {tags {"scripting repl"}} {
    start_server {} {
        test {Before the slave connects we issue an EVAL command} {
            r eval {return redis.call('incr','x')} 0
        } {1}

        test {Connect a slave to the main instance} {
            r -1 slaveof [srv 0 host] [srv 0 port]
            after 1000
            s -1 role
        } {slave}

        test {Now use EVALSHA against the master} {
            r evalsha ae3477e27be955de7e1bc9adfdca626b478d3cb2 0
        } {2}

        if {$::valgrind} {after 2000} else {after 100}

        test {If EVALSHA was replicated as EVAL the slave should be ok} {
            r -1 get x
        } {2}
    }
}
