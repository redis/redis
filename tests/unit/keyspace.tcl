start_server {tags {"keyspace"}} {
    test {DEL against a single item} {
        r set x foo
        assert {[r get x] eq "foo"}
        r del x
        r get x
    } {}

    test {Vararg DEL} {
        r set foo1{t} a
        r set foo2{t} b
        r set foo3{t} c
        list [r del foo1{t} foo2{t} foo3{t} foo4{t}] [r mget foo1{t} foo2{t} foo3{t}]
    } {3 {{} {} {}}}

    test {Untagged multi-key commands} {
        r mset foo1 a foo2 b foo3 c
        assert_equal {a b c {}} [r mget foo1 foo2 foo3 foo4]
        r del foo1 foo2 foo3 foo4
    } {3} {cluster:skip}

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

    test {KEYS with hashtag} {
        foreach key {"{a}x" "{a}y" "{a}z" "{b}a" "{b}b" "{b}c"} {
            r set $key hello
        }
        assert_equal [lsort [r keys "{a}*"]] [list "{a}x" "{a}y" "{a}z"]
        assert_equal [lsort [r keys "*{b}*"]] [list "{b}a" "{b}b" "{b}c"]
    } 

    test {DEL all keys} {
        foreach key [r keys *] {r del $key}
        r dbsize
    } {0}

    test "DEL against expired key" {
        r debug set-active-expire 0
        r setex keyExpire 1 valExpire
        after 1100
        assert_equal 0 [r del keyExpire]
        r debug set-active-expire 1
    } {OK} {needs:debug}

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
        puts -nonewline $fd "SET k1 xyzk\r\nGET k1\r\nPING\r\n"
        flush $fd
        set res {}
        append res [string match OK* [r read]]
        append res [r read]
        append res [string match PONG* [r read]]
        format $res
    } {1xyzk1}

    test {Non existing command} {
        catch {r foobaredcommand} err
        string match ERR* $err
    } {1}

    test {RENAME basic usage} {
        r set mykey{t} hello
        r rename mykey{t} mykey1{t}
        r rename mykey1{t} mykey2{t}
        r get mykey2{t}
    } {hello}

    test {RENAME source key should no longer exist} {
        r exists mykey
    } {0}

    test {RENAME against already existing key} {
        r set mykey{t} a
        r set mykey2{t} b
        r rename mykey2{t} mykey{t}
        set res [r get mykey{t}]
        append res [r exists mykey2{t}]
    } {b0}

    test {RENAMENX basic usage} {
        r del mykey{t}
        r del mykey2{t}
        r set mykey{t} foobar
        r renamenx mykey{t} mykey2{t}
        set res [r get mykey2{t}]
        append res [r exists mykey{t}]
    } {foobar0}

    test {RENAMENX against already existing key} {
        r set mykey{t} foo
        r set mykey2{t} bar
        r renamenx mykey{t} mykey2{t}
    } {0}

    test {RENAMENX against already existing key (2)} {
        set res [r get mykey{t}]
        append res [r get mykey2{t}]
    } {foobar}

    test {RENAME against non existing source key} {
        catch {r rename nokey{t} foobar{t}} err
        format $err
    } {ERR*}

    test {RENAME where source and dest key are the same (existing)} {
        r set mykey foo
        r rename mykey mykey
    } {OK}

    test {RENAMENX where source and dest key are the same (existing)} {
        r set mykey foo
        r renamenx mykey mykey
    } {0}

    test {RENAME where source and dest key are the same (non existing)} {
        r del mykey
        catch {r rename mykey mykey} err
        format $err
    } {ERR*}

    test {RENAME with volatile key, should move the TTL as well} {
        r del mykey{t} mykey2{t}
        r set mykey{t} foo
        r expire mykey{t} 100
        assert {[r ttl mykey{t}] > 95 && [r ttl mykey{t}] <= 100}
        r rename mykey{t} mykey2{t}
        assert {[r ttl mykey2{t}] > 95 && [r ttl mykey2{t}] <= 100}
    }

    test {RENAME with volatile key, should not inherit TTL of target key} {
        r del mykey{t} mykey2{t}
        r set mykey{t} foo
        r set mykey2{t} bar
        r expire mykey2{t} 100
        assert {[r ttl mykey{t}] == -1 && [r ttl mykey2{t}] > 0}
        r rename mykey{t} mykey2{t}
        r ttl mykey2{t}
    } {-1}

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
    } {0} {singledb:skip}

    test {COPY basic usage for string} {
        r set mykey{t} foobar
        set res {}
        r copy mykey{t} mynewkey{t}
        lappend res [r get mynewkey{t}]
        lappend res [r dbsize]
        if {$::singledb} {
            assert_equal [list foobar 2] [format $res]
        } else {
            r copy mykey{t} mynewkey{t} DB 10
            r select 10
            lappend res [r get mynewkey{t}]
            lappend res [r dbsize]
            r select 9
            assert_equal [list foobar 2 foobar 1] [format $res]
        }
    } 

    test {COPY for string does not replace an existing key without REPLACE option} {
        r set mykey2{t} hello
        catch {r copy mykey2{t} mynewkey{t} DB 10} e
        set e
    } {0} {singledb:skip}

    test {COPY for string can replace an existing key with REPLACE option} {
        r copy mykey2{t} mynewkey{t} DB 10 REPLACE
        r select 10
        r get mynewkey{t}
    } {hello} {singledb:skip}

    test {COPY for string ensures that copied data is independent of copying data} {
        r flushdb
        r select 9
        r set mykey{t} foobar
        set res {}
        r copy mykey{t} mynewkey{t} DB 10
        r select 10
        lappend res [r get mynewkey{t}]
        r set mynewkey{t} hoge
        lappend res [r get mynewkey{t}]
        r select 9
        lappend res [r get mykey{t}]
        r select 10
        r flushdb
        r select 9
        format $res
    } [list foobar hoge foobar] {singledb:skip}

    test {COPY for string does not copy data to no-integer DB} {
        r set mykey{t} foobar
        catch {r copy mykey{t} mynewkey{t} DB notanumber} e
        set e
    } {ERR value is not an integer or out of range}

    test {COPY can copy key expire metadata as well} {
        r set mykey{t} foobar ex 100
        r copy mykey{t} mynewkey{t} REPLACE
        assert {[r ttl mynewkey{t}] > 0 && [r ttl mynewkey{t}] <= 100}
        assert {[r get mynewkey{t}] eq "foobar"}
    }

    test {COPY does not create an expire if it does not exist} {
        r set mykey{t} foobar
        assert {[r ttl mykey{t}] == -1}
        r copy mykey{t} mynewkey{t} REPLACE
        assert {[r ttl mynewkey{t}] == -1}
        assert {[r get mynewkey{t}] eq "foobar"}
    }

source "tests/unit/type/list-common.tcl"
foreach {type large} [array get largevalue] {
    set origin_config [config_get_set list-max-listpack-size -1]
    test "COPY basic usage for list - $type" {
        r del mylist{t} mynewlist{t}
        r lpush mylist{t} a b $large c d
        assert_encoding $type mylist{t}
        r copy mylist{t} mynewlist{t}
        assert_encoding $type mynewlist{t}
        set digest [debug_digest_value mylist{t}]
        assert_equal $digest [debug_digest_value mynewlist{t}]
        assert_refcount 1 mylist{t}
        assert_refcount 1 mynewlist{t}
        r del mylist{t}
        assert_equal $digest [debug_digest_value mynewlist{t}]
    }
    config_set list-max-listpack-size $origin_config
}

    foreach type {intset listpack hashtable} {
        test {COPY basic usage for $type set} {
            r del set1{t} newset1{t}
            r sadd set1{t} 1 2 3
            if {$type ne "intset"} {
                r sadd set1{t} a
            }
            if {$type eq "hashtable"} {
                for {set i 4} {$i < 200} {incr i} {
                    r sadd set1{t} $i
                }
            }
            assert_encoding $type set1{t}
            r copy set1{t} newset1{t}
            set digest [debug_digest_value set1{t}]
            assert_equal $digest [debug_digest_value newset1{t}]
            assert_refcount 1 set1{t}
            assert_refcount 1 newset1{t}
            r del set1{t}
            assert_equal $digest [debug_digest_value newset1{t}]
        }
    }

    test {COPY basic usage for listpack sorted set} {
        r del zset1{t} newzset1{t}
        r zadd zset1{t} 123 foobar
        assert_encoding listpack zset1{t}
        r copy zset1{t} newzset1{t}
        set digest [debug_digest_value zset1{t}]
        assert_equal $digest [debug_digest_value newzset1{t}]
        assert_refcount 1 zset1{t}
        assert_refcount 1 newzset1{t}
        r del zset1{t}
        assert_equal $digest [debug_digest_value newzset1{t}]
    }

     test {COPY basic usage for skiplist sorted set} {
        r del zset2{t} newzset2{t}
        set original_max [lindex [r config get zset-max-ziplist-entries] 1]
        r config set zset-max-ziplist-entries 0
        for {set j 0} {$j < 130} {incr j} {
            r zadd zset2{t} [randomInt 50] ele-[randomInt 10]
        }
        assert_encoding skiplist zset2{t}
        r copy zset2{t} newzset2{t}
        set digest [debug_digest_value zset2{t}]
        assert_equal $digest [debug_digest_value newzset2{t}]
        assert_refcount 1 zset2{t}
        assert_refcount 1 newzset2{t}
        r del zset2{t}
        assert_equal $digest [debug_digest_value newzset2{t}]
        r config set zset-max-ziplist-entries $original_max
    }

    test {COPY basic usage for listpack hash} {
        r del hash1{t} newhash1{t}
        r hset hash1{t} tmp 17179869184
        assert_encoding listpack hash1{t}
        r copy hash1{t} newhash1{t}
        set digest [debug_digest_value hash1{t}]
        assert_equal $digest [debug_digest_value newhash1{t}]
        assert_refcount 1 hash1{t}
        assert_refcount 1 newhash1{t}
        r del hash1{t}
        assert_equal $digest [debug_digest_value newhash1{t}]
    }

    test {COPY basic usage for hashtable hash} {
        r del hash2{t} newhash2{t}
        set original_max [lindex [r config get hash-max-ziplist-entries] 1]
        r config set hash-max-ziplist-entries 0
        for {set i 0} {$i < 64} {incr i} {
            r hset hash2{t} [randomValue] [randomValue]
        }
        assert_encoding hashtable hash2{t}
        r copy hash2{t} newhash2{t}
        set digest [debug_digest_value hash2{t}]
        assert_equal $digest [debug_digest_value newhash2{t}]
        assert_refcount 1 hash2{t}
        assert_refcount 1 newhash2{t}
        r del hash2{t}
        assert_equal $digest [debug_digest_value newhash2{t}]
        r config set hash-max-ziplist-entries $original_max
    }

    test {COPY basic usage for stream} {
        r del mystream{t} mynewstream{t}
        for {set i 0} {$i < 1000} {incr i} {
            r XADD mystream{t} * item 2 value b
        }
        r copy mystream{t} mynewstream{t}
        set digest [debug_digest_value mystream{t}]
        assert_equal $digest [debug_digest_value mynewstream{t}]
        assert_refcount 1 mystream{t}
        assert_refcount 1 mynewstream{t}
        r del mystream{t}
        assert_equal $digest [debug_digest_value mynewstream{t}]
    }

    test {COPY basic usage for stream-cgroups} {
        r del x{t}
        r XADD x{t} 100 a 1
        set id [r XADD x{t} 101 b 1]
        r XADD x{t} 102 c 1
        r XADD x{t} 103 e 1
        r XADD x{t} 104 f 1
        r XADD x{t} 105 g 1
        r XGROUP CREATE x{t} g1 0
        r XGROUP CREATE x{t} g2 0
        r XREADGROUP GROUP g1 Alice COUNT 1 STREAMS x{t} >
        r XREADGROUP GROUP g1 Bob COUNT 1 STREAMS x{t} >
        r XREADGROUP GROUP g1 Bob NOACK COUNT 1 STREAMS x{t} >
        r XREADGROUP GROUP g2 Charlie COUNT 4 STREAMS x{t} >
        r XGROUP SETID x{t} g1 $id
        r XREADGROUP GROUP g1 Dave COUNT 3 STREAMS x{t} >
        r XDEL x{t} 103

        r copy x{t} newx{t}
        set info [r xinfo stream x{t} full]
        assert_equal $info [r xinfo stream newx{t} full]
        assert_refcount 1 x{t}
        assert_refcount 1 newx{t}
        r del x{t}
        assert_equal $info [r xinfo stream newx{t} full]
        r flushdb
    }

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
    } [list 0 0 foobar 1] {singledb:skip}

    test {MOVE against key existing in the target DB} {
        r set mykey hello
        r move mykey 10
    } {0} {singledb:skip}

    test {MOVE against non-integer DB (#1428)} {
        r set mykey hello
        catch {r move mykey notanumber} e
        set e
    } {ERR value is not an integer or out of range} {singledb:skip}

    test {MOVE can move key expire metadata as well} {
        r select 10
        r flushdb
        r select 9
        r set mykey foo ex 100
        r move mykey 10
        assert {[r ttl mykey] == -2}
        r select 10
        assert {[r ttl mykey] > 0 && [r ttl mykey] <= 100}
        assert {[r get mykey] eq "foo"}
        r select 9
    } {OK} {singledb:skip}

    test {MOVE does not create an expire if it does not exist} {
        r select 10
        r flushdb
        r select 9
        r set mykey foo
        r move mykey 10
        assert {[r ttl mykey] == -2}
        r select 10
        assert {[r ttl mykey] == -1}
        assert {[r get mykey] eq "foo"}
        r select 9
    } {OK} {singledb:skip}

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
    } {hello world foo bared} {singledb:skip}

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

    test {KEYS * two times with long key, Github issue #1208} {
        r flushdb
        r set dlskeriewrioeuwqoirueioqwrueoqwrueqw test
        r keys *
        r keys *
    } {dlskeriewrioeuwqoirueioqwrueoqwrueqw}

    test {Regression for pattern matching long nested loops} {
        r flushdb
        r SET aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa 1
        r KEYS "a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*b"
    } {}

    test {Coverage: basic SWAPDB test and unhappy path} {
       r flushall
       r select 0
       r set swapkey v1
       r select 1
       assert_match 0 [r dbsize] ;#verify DB[1] has 0 keys
       r swapdb 0 1
       assert_match 1 [r dbsize]
       r select 0
       assert_match 0 [r dbsize] ;#verify DB[0] has 0 keys
       r flushall
       assert_error "ERR DB index is out of range*" {r swapdb 44 55}
       assert_error "ERR invalid second DB index*" {r swapdb 44 a}
       assert_error "ERR invalid first DB index*" {r swapdb a 55}
       assert_error "ERR invalid first DB index*" {r swapdb a b}
       assert_match "OK" [r swapdb 0 0]
    } {} {singledb:skip}

    test {Coverage: SWAPDB and FLUSHDB} {
       # set a key in each db and swapdb one of 2 with different db
       # and flushdb on swapped db.
       r flushall
       r select 0
       r set swapkey v1
       r select 1
       r set swapkey1 v1
       assert_no_match "*db2:keys=*" [r info keyspace]
       r swapdb 0 2
       r select 0
       assert_match 0 [r dbsize]
       assert_no_match "*db0:keys=*" [r info keyspace]
       r select 2
       r flushdb
       assert_match 0 [r dbsize]
       assert_match "*db1:keys=*" [r info keyspace]
       assert_no_match "*db0:keys=*" [r info keyspace]
       assert_no_match "*db2:keys=*" [r info keyspace]
       r flushall
    } {OK} {singledb:skip}
}
