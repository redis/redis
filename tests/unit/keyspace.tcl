start_server {tags {"keyspace"}} {
    test {DEL against a single item} {
        r set x foo
        assert {[r get x] eq "foo"}
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

    test "DEL against expired key" {
        r debug set-active-expire 0
        r setex keyExpire 1 valExpire
        after 1100
        assert_equal 0 [r del keyExpire]
        r debug set-active-expire 1
    }

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
        r del mykey mykey2
        r set mykey foo
        r expire mykey 100
        assert {[r ttl mykey] > 95 && [r ttl mykey] <= 100}
        r rename mykey mykey2
        assert {[r ttl mykey2] > 95 && [r ttl mykey2] <= 100}
    }

    test {RENAME with volatile key, should not inherit TTL of target key} {
        r del mykey mykey2
        r set mykey foo
        r set mykey2 bar
        r expire mykey2 100
        assert {[r ttl mykey] == -1 && [r ttl mykey2] > 0}
        r rename mykey mykey2
        r ttl mykey2
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
    } {0}

    test {COPY basic usage for string} {
        r set mykey foobar
        set res {}
        r copy mykey mynewkey
        lappend res [r get mynewkey]
        lappend res [r dbsize]
        r copy mykey mynewkey DB 10
        r select 10
        lappend res [r get mynewkey]
        lappend res [r dbsize]
        r select 9
        format $res
    } [list foobar 2 foobar 1]

    test {COPY for string does not replace an existing key without REPLACE option} {
        r set mykey2 hello
        catch {r copy mykey2 mynewkey DB 10} e
        set e
    } {0}

    test {COPY for string can replace an existing key with REPLACE option} {
        r copy mykey2 mynewkey DB 10 REPLACE
        r select 10
        r get mynewkey
    } {hello}

    test {COPY for string ensures that copied data is independent of copying data} {
        r flushdb
        r select 9
        r set mykey foobar
        set res {}
        r copy mykey mynewkey DB 10
        r select 10
        lappend res [r get mynewkey]
        r set mynewkey hoge
        lappend res [r get mynewkey]
        r select 9
        lappend res [r get mykey]
        r select 10
        r flushdb
        r select 9
        format $res
    } [list foobar hoge foobar]

    test {COPY for string does not copy data to no-integer DB} {
        r set mykey foobar
        catch {r copy mykey mynewkey DB notanumber} e
        set e
    } {ERR value is not an integer or out of range}

    test {COPY can copy key expire metadata as well} {
        r set mykey foobar ex 100
        r copy mykey mynewkey REPLACE
        assert {[r ttl mynewkey] > 0 && [r ttl mynewkey] <= 100}
        assert {[r get mynewkey] eq "foobar"}
    }

    test {COPY does not create an expire if it does not exist} {
        r set mykey foobar
        assert {[r ttl mykey] == -1}
        r copy mykey mynewkey REPLACE
        assert {[r ttl mynewkey] == -1}
        assert {[r get mynewkey] eq "foobar"}
    }

    test {COPY basic usage for list} {
        r del mylist mynewlist
        r lpush mylist a b c d
        r copy mylist mynewlist
        set digest [r debug digest-value mylist]
        assert_equal $digest [r debug digest-value mynewlist]
        assert_equal 1 [r object refcount mylist]
        assert_equal 1 [r object refcount mynewlist]
        r del mylist
        assert_equal $digest [r debug digest-value mynewlist]
    }

    test {COPY basic usage for intset set} {
        r del set1 newset1 
        r sadd set1 1 2 3
        assert_encoding intset set1
        r copy set1 newset1
        set digest [r debug digest-value set1]
        assert_equal $digest [r debug digest-value newset1]
        assert_equal 1 [r object refcount set1]
        assert_equal 1 [r object refcount newset1]
        r del set1
        assert_equal $digest [r debug digest-value newset1]
    }

    test {COPY basic usage for hashtable set} {
        r del set2 newset2
        r sadd set2 1 2 3 a
        assert_encoding hashtable set2
        r copy set2 newset2
        set digest [r debug digest-value set2]
        assert_equal $digest [r debug digest-value newset2]
        assert_equal 1 [r object refcount set2]
        assert_equal 1 [r object refcount newset2]
        r del set2
        assert_equal $digest [r debug digest-value newset2]
    }

    test {COPY basic usage for ziplist sorted set} {
        r del zset1 newzset1
        r zadd zset1 123 foobar
        assert_encoding ziplist zset1
        r copy zset1 newzset1
        set digest [r debug digest-value zset1]
        assert_equal $digest [r debug digest-value newzset1]
        assert_equal 1 [r object refcount zset1]
        assert_equal 1 [r object refcount newzset1]
        r del zset1
        assert_equal $digest [r debug digest-value newzset1]
    }

     test {COPY basic usage for skiplist sorted set} {
        r del zset2 newzset2
        set original_max [lindex [r config get zset-max-ziplist-entries] 1]
        r config set zset-max-ziplist-entries 0
        for {set j 0} {$j < 130} {incr j} {
            r zadd zset2 [randomInt 50] ele-[randomInt 10]
        }
        assert_encoding skiplist zset2
        r copy zset2 newzset2
        set digest [r debug digest-value zset2]
        assert_equal $digest [r debug digest-value newzset2]
        assert_equal 1 [r object refcount zset2]
        assert_equal 1 [r object refcount newzset2]
        r del zset2
        assert_equal $digest [r debug digest-value newzset2]
        r config set zset-max-ziplist-entries $original_max
    }

    test {COPY basic usage for ziplist hash} {
        r del hash1 newhash1
        r hset hash1 tmp 17179869184
        assert_encoding ziplist hash1
        r copy hash1 newhash1
        set digest [r debug digest-value hash1]
        assert_equal $digest [r debug digest-value newhash1]
        assert_equal 1 [r object refcount hash1]
        assert_equal 1 [r object refcount newhash1]
        r del hash1
        assert_equal $digest [r debug digest-value newhash1]
    }

    test {COPY basic usage for hashtable hash} {
        r del hash2 newhash2
        set original_max [lindex [r config get hash-max-ziplist-entries] 1]
        r config set hash-max-ziplist-entries 0
        for {set i 0} {$i < 64} {incr i} {
            r hset hash2 [randomValue] [randomValue]
        }
        assert_encoding hashtable hash2
        r copy hash2 newhash2
        set digest [r debug digest-value hash2]
        assert_equal $digest [r debug digest-value newhash2]
        assert_equal 1 [r object refcount hash2]
        assert_equal 1 [r object refcount newhash2]
        r del hash2
        assert_equal $digest [r debug digest-value newhash2]
        r config set hash-max-ziplist-entries $original_max
    }

    test {COPY basic usage for stream} {
        r del mystream mynewstream
        for {set i 0} {$i < 1000} {incr i} {
            r XADD mystream * item 2 value b
        }
        r copy mystream mynewstream
        set digest [r debug digest-value mystream]
        assert_equal $digest [r debug digest-value mynewstream]
        assert_equal 1 [r object refcount mystream]
        assert_equal 1 [r object refcount mynewstream]
        r del mystream
        assert_equal $digest [r debug digest-value mynewstream]
    }

    test {COPY basic usage for stream-cgroups} {
        r del x
        r XADD x 100 a 1
        set id [r XADD x 101 b 1]
        r XADD x 102 c 1
        r XADD x 103 e 1
        r XADD x 104 f 1
        r XADD x 105 g 1
        r XGROUP CREATE x g1 0
        r XGROUP CREATE x g2 0
        r XREADGROUP GROUP g1 Alice COUNT 1 STREAMS x >
        r XREADGROUP GROUP g1 Bob COUNT 1 STREAMS x >
        r XREADGROUP GROUP g1 Bob NOACK COUNT 1 STREAMS x >
        r XREADGROUP GROUP g2 Charlie COUNT 4 STREAMS x >
        r XGROUP SETID x g1 $id
        r XREADGROUP GROUP g1 Dave COUNT 3 STREAMS x >
        r XDEL x 103

        r copy x newx
        set info [r xinfo stream x full]
        assert_equal $info [r xinfo stream newx full]
        assert_equal 1 [r object refcount x]
        assert_equal 1 [r object refcount newx]
        r del x
        assert_equal $info [r xinfo stream newx full]
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
    } [list 0 0 foobar 1]

    test {MOVE against key existing in the target DB} {
        r set mykey hello
        r move mykey 10
    } {0}

    test {MOVE against non-integer DB (#1428)} {
        r set mykey hello
        catch {r move mykey notanumber} e
        set e
    } {ERR value is not an integer or out of range}

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
    }

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
    }

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
}
