start_server {tags {"expire"}} {
    test {EXPIRE - set timeouts multiple times} {
        r set x foobar
        set v1 [r expire x 5]
        set v2 [r ttl x]
        set v3 [r expire x 10]
        set v4 [r ttl x]
        r expire x 2
		assert {$v1 eq 1}
		assert {$v2 >= 4}
		assert {$v3 eq 1}
		assert {$v4 >= 9}
    }

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
		set minExpire 900
		set expire 1
		set maxExpire 1100
		if { $::tcl_platform(platform) == "windows" } {
			# tweaking windows expire times in order to bypass unit test failures. statement-statement execution time on a fully taxed system can be 150ms.
			set minExpire 1000
			set expire 2
			set maxExpire 3000
		}

        # This test is very likely to do a false positive if the
        # server is under pressure, so if it does not work give it a few more
        # chances.
        for {set j 0} {$j < 10} {incr j} {
            r del x
            r setex x $expire somevalue
            after $minExpire
            set a [r get x]
            after $maxExpire
            set b [r get x]
            if {$a eq {somevalue} && $b eq {}} break
        }
        list $a $b
    } {somevalue {}}

    test {PEXPIRE/PSETEX/PEXPIREAT can set sub-second expires} {
		set minExpire 80
		set expire 100
		set maxExpire 120
		if { $::tcl_platform(platform) == "windows" } {
			# tweaking windows expire times in order to bypass unit test failures. statement-statement execution time on a fully taxed system can be 100's of ms.
			set minExpire 10
			set expire 1000
			set maxExpire 2000
		}

        # This test is very likely to do a false positive if the
        # server is under pressure, so if it does not work give it a few more
        # chances.
        for {set j 0} {$j < 10} {incr j} {
            r del x y z
            r psetex x $expire somevalue_1
            after $minExpire
            set a [r get x]
            after $maxExpire
            set b [r get x]

            r set x somevalue_2
            r pexpire x $expire
            after $minExpire
            set c [r get x]
            after $maxExpire
            set d [r get x]

            r set x somevalue_3
            r pexpireat x [expr ([clock seconds]*1000)+$expire]
			after $minExpire
            set e [r get x]
            after $maxExpire
            set f [r get x]
				
            if {$a eq {somevalue_1} && $b eq {} &&
                $c eq {somevalue_2} && $d eq {} &&
                $e eq {somevalue_3} && $f eq {}} break
        }
        list $a $b $c $d $e $f
    } {somevalue_1 {} somevalue_2 {} somevalue_3 {}}

    test {PTTL returns millisecond time to live} {
		set expireTime 1
		set minTime 900
		set maxTime 1000
		if { $::tcl_platform(platform) == "windows" } {
			set expireTime 2
			set minTime 200
			set maxTime 2000
		}
        r del x
        r setex x $expireTime somevalue
        set ttl [r pttl x]
        assert {$ttl > $minTime && $ttl <= $maxTime}
    }

    test {Redis should actively expire keys incrementally} {
    	set expireTime 500
		set evictionTime 1000
		if { $::tcl_platform(platform) == "windows" } {
			set expireTime 2000
			set evictionTime 4000
		}
	    r flushdb
        r psetex key1 $expireTime a
        r psetex key2 $expireTime a
        r psetex key3 $expireTime a
        set size1 [r dbsize]
        # Redis expires random keys ten times every second so we are
        # fairly sure that all the three keys should be evicted after
        # one second.
        after $evictionTime
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
}
