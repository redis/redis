start_server {tags {basic}} {
    test {DEL all keys to start with a clean DB} {
        foreach key [r keys *] {r del $key}
        r dbsize
    } {0}

    test {SET and GET an item} {
        r set x foobar
        r get x
    } {foobar}

    test {SET and GET an empty item} {
        r set x {}
        r get x
    } {}

    test {DEL against a single item} {
        r del x
        r get x
    } {}

    test {Vararg DEL} {
        r set foo1 a
        r set foo2 b
        r set foo3 c
        list [r del foo1 foo2 foo3 foo4] [r mget foo1 foo2 foo3]
    } {3 {{} {} {}}}

    test {KEYS with pattern} {
        foreach key {key_x key_y key_z foo_a foo_b foo_c} {
            r set $key hello
        }
        lsort [r keys foo*]
    } {foo_a foo_b foo_c}

    test {KEYS to get all keys} {
        lsort [r keys *]
    } {foo_a foo_b foo_c key_x key_y key_z}

    test {DBSIZE} {
        r dbsize
    } {6}

    test {DEL all keys} {
        foreach key [r keys *] {r del $key}
        r dbsize
    } {0}

    test {Very big payload in GET/SET} {
        set buf [string repeat "abcd" 1000000]
        r set foo $buf
        r get foo
    } [string repeat "abcd" 1000000]

    tags {slow} {
    test {Very big payload random access} {
        set err {}
        array set payload {}
        for {set j 0} {$j < 100} {incr j} {
            set size [expr 1+[randomInt 100000]]
            set buf [string repeat "pl-$j" $size]
            set payload($j) $buf
            r set bigpayload_$j $buf
        }
        for {set j 0} {$j < 1000} {incr j} {
            set index [randomInt 100]
            set buf [r get bigpayload_$index]
            if {$buf != $payload($index)} {
                set err "Values differ: I set '$payload($index)' but I read back '$buf'"
                break
            }
        }
        unset payload
        set _ $err
    } {}

    test {SET 10000 numeric keys and access all them in reverse order} {
        set err {}
        for {set x 0} {$x < 10000} {incr x} {
            r set $x $x
        }
        set sum 0
        for {set x 9999} {$x >= 0} {incr x -1} {
            set val [r get $x]
            if {$val ne $x} {
                set err "Eleemnt at position $x is $val instead of $x"
                break
            }
        }
        set _ $err
    } {}

    test {DBSIZE should be 10101 now} {
        r dbsize
    } {10101}
    }

    test {INCR against non existing key} {
        set res {}
        append res [r incr novar]
        append res [r get novar]
    } {11}

    test {INCR against key created by incr itself} {
        r incr novar
    } {2}

    test {INCR against key originally set with SET} {
        r set novar 100
        r incr novar
    } {101}

    test {INCR over 32bit value} {
        r set novar 17179869184
        r incr novar
    } {17179869185}

    test {INCRBY over 32bit value with over 32bit increment} {
        r set novar 17179869184
        r incrby novar 17179869184
    } {34359738368}

    test {INCR fails against key with spaces (no integer encoded)} {
        r set novar "    11    "
        catch {r incr novar} err
        format $err
    } {ERR*}

    test {INCR fails against a key holding a list} {
        r rpush mylist 1
        catch {r incr mylist} err
        r rpop mylist
        format $err
    } {ERR*}

    test {DECRBY over 32bit value with over 32bit increment, negative res} {
        r set novar 17179869184
        r decrby novar 17179869185
    } {-1}

    test {SETNX target key missing} {
        r setnx novar2 foobared
        r get novar2
    } {foobared}

    test {SETNX target key exists} {
        r setnx novar2 blabla
        r get novar2
    } {foobared}

    test {SETNX will overwrite EXPIREing key} {
        r set x 10
        r expire x 10000
        r setnx x 20
        r get x
    } {20}

    test {EXISTS} {
        set res {}
        r set newkey test
        append res [r exists newkey]
        r del newkey
        append res [r exists newkey]
    } {10}

    test {Zero length value in key. SET/GET/EXISTS} {
        r set emptykey {}
        set res [r get emptykey]
        append res [r exists emptykey]
        r del emptykey
        append res [r exists emptykey]
    } {10}

    test {Commands pipelining} {
        set fd [r channel]
        puts -nonewline $fd "SET k1 4\r\nxyzk\r\nGET k1\r\nPING\r\n"
        flush $fd
        set res {}
        append res [string match OK* [::redis::redis_read_reply $fd]]
        append res [::redis::redis_read_reply $fd]
        append res [string match PONG* [::redis::redis_read_reply $fd]]
        format $res
    } {1xyzk1}

    test {Non existing command} {
        catch {r foobaredcommand} err
        string match ERR* $err
    } {1}
    
    test {RENAME basic usage} {
        r set mykey hello
        r rename mykey mykey1
        r rename mykey1 mykey2
        r get mykey2
    } {hello}

    test {RENAME source key should no longer exist} {
        r exists mykey
    } {0}

    test {RENAME against already existing key} {
        r set mykey a
        r set mykey2 b
        r rename mykey2 mykey
        set res [r get mykey]
        append res [r exists mykey2]
    } {b0}

    test {RENAMENX basic usage} {
        r del mykey
        r del mykey2
        r set mykey foobar
        r renamenx mykey mykey2
        set res [r get mykey2]
        append res [r exists mykey]
    } {foobar0}

    test {RENAMENX against already existing key} {
        r set mykey foo
        r set mykey2 bar
        r renamenx mykey mykey2
    } {0}

    test {RENAMENX against already existing key (2)} {
        set res [r get mykey]
        append res [r get mykey2]
    } {foobar}

    test {RENAME against non existing source key} {
        catch {r rename nokey foobar} err
        format $err
    } {ERR*}

    test {RENAME where source and dest key is the same} {
        catch {r rename mykey mykey} err
        format $err
    } {ERR*}

    test {DEL all keys again (DB 0)} {
        foreach key [r keys *] {
            r del $key
        }
        r dbsize
    } {0}

    test {DEL all keys again (DB 1)} {
        r select 10
        foreach key [r keys *] {
            r del $key
        }
        set res [r dbsize]
        r select 9
        format $res
    } {0}

    test {MOVE basic usage} {
        r set mykey foobar
        r move mykey 10
        set res {}
        lappend res [r exists mykey]
        lappend res [r dbsize]
        r select 10
        lappend res [r get mykey]
        lappend res [r dbsize]
        r select 9
        format $res
    } [list 0 0 foobar 1]

    test {MOVE against key existing in the target DB} {
        r set mykey hello
        r move mykey 10
    } {0}

    test {SET/GET keys in different DBs} {
        r set a hello
        r set b world
        r select 10
        r set a foo
        r set b bared
        r select 9
        set res {}
        lappend res [r get a]
        lappend res [r get b]
        r select 10
        lappend res [r get a]
        lappend res [r get b]
        r select 9
        format $res
    } {hello world foo bared}
    
    test {MGET} {
        r flushdb
        r set foo BAR
        r set bar FOO
        r mget foo bar
    } {BAR FOO}

    test {MGET against non existing key} {
        r mget foo baazz bar
    } {BAR {} FOO}

    test {MGET against non-string key} {
        r sadd myset ciao
        r sadd myset bau
        r mget foo baazz bar myset
    } {BAR {} FOO {}}

    test {RANDOMKEY} {
        r flushdb
        r set foo x
        r set bar y
        set foo_seen 0
        set bar_seen 0
        for {set i 0} {$i < 100} {incr i} {
            set rkey [r randomkey]
            if {$rkey eq {foo}} {
                set foo_seen 1
            }
            if {$rkey eq {bar}} {
                set bar_seen 1
            }
        }
        list $foo_seen $bar_seen
    } {1 1}

    test {RANDOMKEY against empty DB} {
        r flushdb
        r randomkey
    } {}

    test {RANDOMKEY regression 1} {
        r flushdb
        r set x 10
        r del x
        r randomkey
    } {}

    test {GETSET (set new value)} {
        list [r getset foo xyz] [r get foo]
    } {{} xyz}

    test {GETSET (replace old value)} {
        r set foo bar
        list [r getset foo xyz] [r get foo]
    } {bar xyz}
    
    test {MSET base case} {
        r mset x 10 y "foo bar" z "x x x x x x x\n\n\r\n"
        r mget x y z
    } [list 10 {foo bar} "x x x x x x x\n\n\r\n"]

    test {MSET wrong number of args} {
        catch {r mset x 10 y "foo bar" z} err
        format $err
    } {*wrong number*}

    test {MSETNX with already existent key} {
        list [r msetnx x1 xxx y2 yyy x 20] [r exists x1] [r exists y2]
    } {0 0 0}

    test {MSETNX with not existing keys} {
        list [r msetnx x1 xxx y2 yyy] [r get x1] [r get y2]
    } {1 xxx yyy}

    test {MSETNX should remove all the volatile keys even on failure} {
        r mset x 1 y 2 z 3
        r expire y 10000
        r expire z 10000
        list [r msetnx x A y B z C] [r mget x y z]
    } {0 {1 {} {}}}
}
