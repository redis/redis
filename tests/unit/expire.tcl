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

    test {EXPIRE pricision is now the millisecond} {
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
        for {set j 0} {$j < 3} {incr j} {
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
            r pexpireat x [expr ([clock seconds]*1000)+100]
            after 80
            set e [r get x]
            after 120
            set f [r get x]

            if {$a eq {somevalue} && $b eq {} &&
                $c eq {somevalue} && $d eq {} &&
                $e eq {somevalue} && $f eq {}} break
        }
        list $a $b
    } {somevalue {}}

    test {TTL returns tiem to live in seconds} {
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

    test {SET - use EX/PX option, TTL should not be reseted after loadaof} {
        r config set appendonly yes
        r set foo bar EX 100
        after 2000
        r debug loadaof
        set ttl [r ttl foo]
        assert {$ttl <= 98 && $ttl > 90}

        r set foo bar PX 100000
        after 2000
        r debug loadaof
        set ttl [r ttl foo]
        assert {$ttl <= 98 && $ttl > 90}
    }
}
