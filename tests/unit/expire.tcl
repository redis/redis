start_server {tags {"expire"}} {
    test {EXPIRE - set timeouts multiple times} {
        r set x foobar
        set v1 [r expire x 5]
        set v2 [r ttl x]
        set v3 [r expire x 10]
        set v4 [r ttl x]
        r expire x 2
        list $v1 $v2 $v3 $v4
    } {1 [45] 1 10}

    test {EXPIRE - It should be still possible to read 'x'} {
        r get x
    } {foobar}

    tags {"slow"} {
        test {EXPIRE - After 2.1 seconds the key should no longer be here} {
            after 2100
            list [r get x] [r exists x]
        } {{} 0}
    }

    test {EXPIRE - write on expire should work} {
        r del x
        r lpush x foo
        r expire x 1000
        r lpush x bar
        r lrange x 0 -1
    } {bar foo}

    test {EXPIREAT - Check for EXPIRE alike behavior} {
        r del x
        r set x foo
        r expireat x [expr [clock seconds]+15]
        r ttl x
    } {1[345]}

    test {SETEX - Set + Expire combo operation. Check for TTL} {
        r setex x 12 test
        r ttl x
    } {1[012]}

    test {SETEX - Check value} {
        r get x
    } {test}

    test {SETEX - Overwrite old key} {
        r setex y 1 foo
        r get y
    } {foo}

    tags {"slow"} {
        test {SETEX - Wait for the key to expire} {
            after 1100
            r get y
        } {}
    }

    test {SETEX - Wrong time parameter} {
        catch {r setex z -10 foo} e
        set _ $e
    } {*invalid expire*}

    test {PERSIST can undo an EXPIRE} {
        r set x foo
        r expire x 50
        list [r ttl x] [r persist x] [r ttl x] [r get x]
    } {50 1 -1 foo}

    test {PERSIST returns 0 against non existing or non volatile keys} {
        r set x foo
        list [r persist foo] [r persist nokeyatall]
    } {0 0}

    test {EXPIRE precision is now the millisecond} {
        # This test is very likely to do a false positive if the
        # server is under pressure, so if it does not work give it a few more
        # chances.
        for {set j 0} {$j < 3} {incr j} {
            r del x
            r setex x 1 somevalue
            after 900
            set a [r get x]
            after 1100
            set b [r get x]
            if {$a eq {somevalue} && $b eq {}} break
        }
        list $a $b
    } {somevalue {}}

    test {PEXPIRE/PSETEX/PEXPIREAT can set sub-second expires} {
        # This test is very likely to do a false positive if the
        # server is under pressure, so if it does not work give it a few more
        # chances.
        for {set j 0} {$j < 30} {incr j} {
            r del x y z
            r psetex x 100 somevalue
            after 80
            set a [r get x]
            after 120
            set b [r get x]

            r set x somevalue
            r pexpire x 100
            after 80
            set c [r get x]
            after 120
            set d [r get x]

            r set x somevalue
            set now [r time]
            r pexpireat x [expr ([lindex $now 0]*1000)+([lindex $now 1]/1000)+200]
            after 20
            set e [r get x]
            after 220
            set f [r get x]

            if {$a eq {somevalue} && $b eq {} &&
                $c eq {somevalue} && $d eq {} &&
                $e eq {somevalue} && $f eq {}} break
        }
        if {$::verbose} {
            puts "sub-second expire test attempts: $j"
        }
        list $a $b $c $d $e $f
    } {somevalue {} somevalue {} somevalue {}}

    test {TTL returns time to live in seconds} {
        r del x
        r setex x 10 somevalue
        set ttl [r ttl x]
        assert {$ttl > 8 && $ttl <= 10}
    }

    test {PTTL returns time to live in milliseconds} {
        r del x
        r setex x 1 somevalue
        set ttl [r pttl x]
        assert {$ttl > 900 && $ttl <= 1000}
    }

    test {TTL / PTTL return -1 if key has no expire} {
        r del x
        r set x hello
        list [r ttl x] [r pttl x]
    } {-1 -1}

    test {TTL / PTTL return -2 if key does not exit} {
        r del x
        list [r ttl x] [r pttl x]
    } {-2 -2}

    test {Redis should actively expire keys incrementally} {
        r flushdb
        r psetex key1 500 a
        r psetex key2 500 a
        r psetex key3 500 a
        set size1 [r dbsize]
        # Redis expires random keys ten times every second so we are
        # fairly sure that all the three keys should be evicted after
        # one second.
        after 1000
        set size2 [r dbsize]
        list $size1 $size2
    } {3 0}

    test {Redis should lazy expire keys} {
        r flushdb
        r debug set-active-expire 0
        r psetex key1 500 a
        r psetex key2 500 a
        r psetex key3 500 a
        set size1 [r dbsize]
        # Redis expires random keys ten times every second so we are
        # fairly sure that all the three keys should be evicted after
        # one second.
        after 1000
        set size2 [r dbsize]
        r mget key1 key2 key3
        set size3 [r dbsize]
        r debug set-active-expire 1
        list $size1 $size2 $size3
    } {3 3 0}

    test {EXPIRE should not resurrect keys (issue #1026)} {
        r debug set-active-expire 0
        r set foo bar
        r pexpire foo 500
        after 1000
        r expire foo 10
        r debug set-active-expire 1
        r exists foo
    } {0}

    test {5 keys in, 5 keys out} {
        r flushdb
        r set a c
        r expire a 5
        r set t c
        r set e c
        r set s c
        r set foo b
        lsort [r keys *]
    } {a e foo s t}

    test {EXPIRE with empty string as TTL should report an error} {
        r set foo bar
        catch {r expire foo ""} e
        set e
    } {*not an integer*}

    test {EXPIRE and SET/GETEX EX/PX/EXAT/PXAT option, TTL should not be reset after loadaof} {
        # This test makes sure that expire times are propagated as absolute
        # times to the AOF file and not as relative time, so that when the AOF
        # is reloaded the TTLs are not being shifted forward to the future.
        # We want the time to logically pass when the server is restarted!

        r config set appendonly yes
        r set foo1 bar EX 100
        r set foo2 bar PX 100000
        r set foo3 bar
        r set foo4 bar
        r expire foo3 100
        r pexpire foo4 100000
        r setex foo5 100 bar
        r psetex foo6 100000 bar
        r set foo7 bar EXAT [expr [clock seconds] + 100]
        r set foo8 bar PXAT [expr [clock milliseconds] + 100000]
        r set foo9 bar
        r getex foo9 EX 100
        r set foo10 bar
        r getex foo10 PX 100000
        r set foo11 bar
        r getex foo11 EXAT [expr [clock seconds] + 100]
        r set foo12 bar
        r getex foo12 PXAT [expr [clock milliseconds] + 100000]

        after 2000
        r debug loadaof
        assert_range [r ttl foo1] 90 98
        assert_range [r ttl foo2] 90 98
        assert_range [r ttl foo3] 90 98
        assert_range [r ttl foo4] 90 98
        assert_range [r ttl foo5] 90 98
        assert_range [r ttl foo6] 90 98
        assert_range [r ttl foo7] 90 98
        assert_range [r ttl foo8] 90 98
        assert_range [r ttl foo9] 90 98
        assert_range [r ttl foo10] 90 98
        assert_range [r ttl foo11] 90 98
        assert_range [r ttl foo12] 90 98
    }

    test {EXPIRE relative and absolute propagation to replicas} {
        # Make sure that relative and absolute expire commands are propagated
        # "as is" to replicas.
        # We want replicas to honor the same high level contract of expires that
        # the master has, that is, we want the time to be counted logically
        # starting from the moment the write was received. This usually provides
        # the most coherent behavior from the point of view of the external
        # users, with TTLs that are similar from the POV of the external observer.
        #
        # This test is here to stop some innocent / eager optimization or cleanup
        # from doing the wrong thing without proper discussion, see:
        # https://github.com/redis/redis/pull/5171#issuecomment-409553266

        set repl [attach_to_replication_stream]
        r set foo1 bar ex 200
        r set foo1 bar px 100000
        r set foo1 bar exat [expr [clock seconds]+100]
        r set foo1 bar pxat [expr [clock milliseconds]+10000]
        r setex foo1 100 bar
        r psetex foo1 100000 bar
        r set foo2 bar
        r expire foo2 100
        r pexpire foo2 100000
        r set foo3 bar
        r expireat foo3 [expr [clock seconds]+100]
        r pexpireat foo3 [expr [clock seconds]*1000+100000]
        r expireat foo3 [expr [clock seconds]-100]
        r set foo4 bar
        r getex foo4 ex 200
        r getex foo4 px 200000
        r getex foo4 exat [expr [clock seconds]+100]
        r getex foo4 pxat [expr [clock milliseconds]+10000]
        assert_replication_stream $repl {
            {select *}
            {set foo1 bar PX 200000}
            {set foo1 bar PX 100000}
            {set foo1 bar PXAT *}
            {set foo1 bar PXAT *}
            {set foo1 bar PX 100000}
            {set foo1 bar PX 100000}
            {set foo2 bar}
            {expire foo2 100}
            {pexpire foo2 100000}
            {set foo3 bar}
            {expireat foo3 *}
            {pexpireat foo3 *}
            {del foo3}
            {set foo4 bar}
            {pexpire foo4 200000}
            {pexpire foo4 200000}
            {pexpireat foo4 *}
            {pexpireat foo4 *}
        }
    }

    test {SET command will remove expire} {
        r set foo bar EX 100
        r set foo bar
        r ttl foo
    } {-1}

    test {SET - use KEEPTTL option, TTL should not be removed} {
        r set foo bar EX 100
        r set foo bar KEEPTTL
        set ttl [r ttl foo]
        assert {$ttl <= 100 && $ttl > 90}
    }

    test {SET - use KEEPTTL option, TTL should not be removed after loadaof} {
        r config set appendonly yes
        r set foo bar EX 100
        r set foo bar2 KEEPTTL
        after 2000
        r debug loadaof
        set ttl [r ttl foo]
        assert {$ttl <= 98 && $ttl > 90}
    }

    test {GETEX use of PERSIST option should remove TTL} {
       r set foo bar EX 100
       r getex foo PERSIST
       r ttl foo
    } {-1}

    test {GETEX use of PERSIST option should remove TTL after loadaof} {
       r set foo bar EX 100
       r getex foo PERSIST
       after 2000
       r debug loadaof
       r ttl foo
    } {-1}

    test {GETEX propagate as to replica as PERSIST, DEL, or nothing} {
       set repl [attach_to_replication_stream]
       r set foo bar EX 100
       r getex foo PERSIST
       r getex foo
       r getex foo exat [expr [clock seconds]-100]
       assert_replication_stream $repl {
           {select *}
           {set foo bar PX 100000}
           {persist foo}
           {del foo}
        }
    }
}
