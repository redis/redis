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
        for {set j 0} {$j < 30} {incr j} {
            r del x
            r setex x 1 somevalue
            after 800
            set a [r get x]
            if {$a ne {somevalue}} continue
            after 300
            set b [r get x]
            if {$b eq {}} break
        }
        if {$::verbose} {
            puts "millisecond expire test attempts: $j"
        }
        assert_equal $a {somevalue}
        assert_equal $b {}
    }

    test "PSETEX can set sub-second expires" {
        # This test is very likely to do a false positive if the server is
        # under pressure, so if it does not work give it a few more chances.
        for {set j 0} {$j < 50} {incr j} {
            r del x
            r psetex x 100 somevalue
            set a [r get x]
            after 101
            set b [r get x]

            if {$a eq {somevalue} && $b eq {}} break
        }
        if {$::verbose} { puts "PSETEX sub-second expire test attempts: $j" }
        list $a $b
    } {somevalue {}}

    test "PEXPIRE can set sub-second expires" {
        # This test is very likely to do a false positive if the server is
        # under pressure, so if it does not work give it a few more chances.
        for {set j 0} {$j < 50} {incr j} {
            r set x somevalue
            r pexpire x 100
            set c [r get x]
            after 101
            set d [r get x]

            if {$c eq {somevalue} && $d eq {}} break
        }
        if {$::verbose} { puts "PEXPIRE sub-second expire test attempts: $j" }
        list $c $d
    } {somevalue {}}

    test "PEXPIREAT can set sub-second expires" {
        # This test is very likely to do a false positive if the server is
        # under pressure, so if it does not work give it a few more chances.
        for {set j 0} {$j < 50} {incr j} {
            r set x somevalue
            set now [r time]
            r pexpireat x [expr ([lindex $now 0]*1000)+([lindex $now 1]/1000)+200]
            set e [r get x]
            after 201
            set f [r get x]

            if {$e eq {somevalue} && $f eq {}} break
        }
        if {$::verbose} { puts "PEXPIREAT sub-second expire test attempts: $j" }
        list $e $f
    } {somevalue {}}

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
        assert {$ttl > 500 && $ttl <= 1000}
    }

    test {TTL / PTTL / EXPIRETIME / PEXPIRETIME return -1 if key has no expire} {
        r del x
        r set x hello
        list [r ttl x] [r pttl x] [r expiretime x] [r pexpiretime x]
    } {-1 -1 -1 -1}

    test {TTL / PTTL / EXPIRETIME / PEXPIRETIME return -2 if key does not exit} {
        r del x
        list [r ttl x] [r pttl x] [r expiretime x] [r pexpiretime x]
    } {-2 -2 -2 -2}

    test {EXPIRETIME returns absolute expiration time in seconds} {
        r del x
        set abs_expire [expr [clock seconds] + 100]
        r set x somevalue exat $abs_expire
        assert_equal [r expiretime x] $abs_expire
    }

    test {PEXPIRETIME returns absolute expiration time in milliseconds} {
        r del x
        set abs_expire [expr [clock milliseconds] + 100000]
        r set x somevalue pxat $abs_expire
        assert_equal [r pexpiretime x] $abs_expire
    }

    test {Redis should actively expire keys incrementally} {
        r flushdb
        r psetex key1 500 a
        r psetex key2 500 a
        r psetex key3 500 a
        assert_equal 3 [r dbsize]
        # Redis expires random keys ten times every second so we are
        # fairly sure that all the three keys should be evicted after
        # two seconds.
        wait_for_condition 20 100 {
            [r dbsize] eq 0
        } else {
            fail "Keys did not actively expire."
        }
    }

    test {Redis should lazy expire keys} {
        r flushdb
        r debug set-active-expire 0
        r psetex key1{t} 500 a
        r psetex key2{t} 500 a
        r psetex key3{t} 500 a
        set size1 [r dbsize]
        # Redis expires random keys ten times every second so we are
        # fairly sure that all the three keys should be evicted after
        # one second.
        after 1000
        set size2 [r dbsize]
        r mget key1{t} key2{t} key3{t}
        set size3 [r dbsize]
        r debug set-active-expire 1
        list $size1 $size2 $size3
    } {3 3 0} {needs:debug}

    test {EXPIRE should not resurrect keys (issue #1026)} {
        r debug set-active-expire 0
        r set foo bar
        r pexpire foo 500
        after 1000
        r expire foo 10
        r debug set-active-expire 1
        r exists foo
    } {0} {needs:debug}

    test {5 keys in, 5 keys out} {
        r flushdb
        r set a c
        r expire a 5
        r set t c
        r set e c
        r set s c
        r set foo b
        assert_equal [lsort [r keys *]] {a e foo s t}
        r del a ; # Do not leak volatile keys to other tests
    }

    test {EXPIRE with empty string as TTL should report an error} {
        r set foo bar
        catch {r expire foo ""} e
        set e
    } {*not an integer*}

    test {SET with EX with big integer should report an error} {
        catch {r set foo bar EX 10000000000000000} e
        set e
    } {ERR invalid expire time in 'set' command}

    test {SET with EX with smallest integer should report an error} {
        catch {r SET foo bar EX -9999999999999999} e
        set e
    } {ERR invalid expire time in 'set' command}

    test {GETEX with big integer should report an error} {
        r set foo bar
        catch {r GETEX foo EX 10000000000000000} e
        set e
    } {ERR invalid expire time in 'getex' command}

    test {GETEX with smallest integer should report an error} {
        r set foo bar
        catch {r GETEX foo EX -9999999999999999} e
        set e
    } {ERR invalid expire time in 'getex' command}

    test {EXPIRE with big integer overflows when converted to milliseconds} {
        r set foo bar

        # Hit `when > LLONG_MAX - basetime`
        assert_error "ERR invalid expire time in 'expire' command" {r EXPIRE foo 9223370399119966}

        # Hit `when > LLONG_MAX / 1000`
        assert_error "ERR invalid expire time in 'expire' command" {r EXPIRE foo 9223372036854776}
        assert_error "ERR invalid expire time in 'expire' command" {r EXPIRE foo 10000000000000000}
        assert_error "ERR invalid expire time in 'expire' command" {r EXPIRE foo 18446744073709561}

        assert_equal {-1} [r ttl foo]
    }

    test {PEXPIRE with big integer overflow when basetime is added} {
        r set foo bar
        catch {r PEXPIRE foo 9223372036854770000} e
        set e
    } {ERR invalid expire time in 'pexpire' command}

    test {EXPIRE with big negative integer} {
        r set foo bar

        # Hit `when < LLONG_MIN / 1000`
        assert_error "ERR invalid expire time in 'expire' command" {r EXPIRE foo -9223372036854776}
        assert_error "ERR invalid expire time in 'expire' command" {r EXPIRE foo -9999999999999999}

        r ttl foo
    } {-1}

    test {PEXPIREAT with big integer works} {
        r set foo bar
        r PEXPIREAT foo 9223372036854770000
    } {1}

    test {PEXPIREAT with big negative integer works} {
        r set foo bar
        r PEXPIREAT foo -9223372036854770000
        r ttl foo
    } {-2}

    # Start a new server with empty data and AOF file.
    start_server {overrides {appendonly {yes} appendfsync always} tags {external:skip}} {
        test {All time-to-live(TTL) in commands are propagated as absolute timestamp in milliseconds in AOF} {
            # This test makes sure that expire times are propagated as absolute
            # times to the AOF file and not as relative time, so that when the AOF
            # is reloaded the TTLs are not being shifted forward to the future.
            # We want the time to logically pass when the server is restarted!

            set aof [get_last_incr_aof_path r]

            # Apply each TTL-related command to a unique key
            # SET commands
            r set foo1 bar ex 100
            r set foo2 bar px 100000
            r set foo3 bar exat [expr [clock seconds]+100]
            r set foo4 bar PXAT [expr [clock milliseconds]+100000]
            r setex foo5 100 bar
            r psetex foo6 100000 bar
            # EXPIRE-family commands
            r set foo7 bar
            r expire foo7 100
            r set foo8 bar
            r pexpire foo8 100000
            r set foo9 bar
            r expireat foo9 [expr [clock seconds]+100]
            r set foo10 bar
            r pexpireat foo10 [expr [clock seconds]*1000+100000]
            r set foo11 bar
            r expireat foo11 [expr [clock seconds]-100]
            # GETEX commands
            r set foo12 bar
            r getex foo12 ex 100
            r set foo13 bar
            r getex foo13 px 100000
            r set foo14 bar
            r getex foo14 exat [expr [clock seconds]+100]
            r set foo15 bar
            r getex foo15 pxat [expr [clock milliseconds]+100000]
            # RESTORE commands
            r set foo16 bar
            set encoded [r dump foo16]
            r restore foo17 100000 $encoded
            r restore foo18 [expr [clock milliseconds]+100000] $encoded absttl

            # Assert that each TTL-related command are persisted with absolute timestamps in AOF
            assert_aof_content $aof {
                {select *}
                {set foo1 bar PXAT *}
                {set foo2 bar PXAT *}
                {set foo3 bar PXAT *}
                {set foo4 bar PXAT *}
                {set foo5 bar PXAT *}
                {set foo6 bar PXAT *}
                {set foo7 bar}
                {pexpireat foo7 *}
                {set foo8 bar}
                {pexpireat foo8 *}
                {set foo9 bar}
                {pexpireat foo9 *}
                {set foo10 bar}
                {pexpireat foo10 *}
                {set foo11 bar}
                {del foo11}
                {set foo12 bar}
                {pexpireat foo12 *}
                {set foo13 bar}
                {pexpireat foo13 *}
                {set foo14 bar}
                {pexpireat foo14 *}
                {set foo15 bar}
                {pexpireat foo15 *}
                {set foo16 bar}
                {restore foo17 * * ABSTTL}
                {restore foo18 * * absttl}
            }

            # Remember the absolute TTLs of all the keys
            set ttl1 [r pexpiretime foo1]
            set ttl2 [r pexpiretime foo2]
            set ttl3 [r pexpiretime foo3]
            set ttl4 [r pexpiretime foo4]
            set ttl5 [r pexpiretime foo5]
            set ttl6 [r pexpiretime foo6]
            set ttl7 [r pexpiretime foo7]
            set ttl8 [r pexpiretime foo8]
            set ttl9 [r pexpiretime foo9]
            set ttl10 [r pexpiretime foo10]
            assert_equal "-2" [r pexpiretime foo11] ; # foo11 is gone
            set ttl12 [r pexpiretime foo12]
            set ttl13 [r pexpiretime foo13]
            set ttl14 [r pexpiretime foo14]
            set ttl15 [r pexpiretime foo15]
            assert_equal "-1" [r pexpiretime foo16] ; # foo16 has no TTL
            set ttl17 [r pexpiretime foo17]
            set ttl18 [r pexpiretime foo18]

            # Let some time pass and reload data from AOF
            after 2000
            r debug loadaof

            # Assert that relative TTLs are roughly the same
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
            assert_equal [r ttl foo11] "-2" ; # foo11 is gone
            assert_range [r ttl foo12] 90 98
            assert_range [r ttl foo13] 90 98
            assert_range [r ttl foo14] 90 98
            assert_range [r ttl foo15] 90 98
            assert_equal [r ttl foo16] "-1" ; # foo16 has no TTL
            assert_range [r ttl foo17] 90 98
            assert_range [r ttl foo18] 90 98

            # Assert that all keys have restored the same absolute TTLs from AOF
            assert_equal [r pexpiretime foo1] $ttl1
            assert_equal [r pexpiretime foo2] $ttl2
            assert_equal [r pexpiretime foo3] $ttl3
            assert_equal [r pexpiretime foo4] $ttl4
            assert_equal [r pexpiretime foo5] $ttl5
            assert_equal [r pexpiretime foo6] $ttl6
            assert_equal [r pexpiretime foo7] $ttl7
            assert_equal [r pexpiretime foo8] $ttl8
            assert_equal [r pexpiretime foo9] $ttl9
            assert_equal [r pexpiretime foo10] $ttl10
            assert_equal [r pexpiretime foo11] "-2" ; # foo11 is gone
            assert_equal [r pexpiretime foo12] $ttl12
            assert_equal [r pexpiretime foo13] $ttl13
            assert_equal [r pexpiretime foo14] $ttl14
            assert_equal [r pexpiretime foo15] $ttl15
            assert_equal [r pexpiretime foo16] "-1" ; # foo16 has no TTL
            assert_equal [r pexpiretime foo17] $ttl17
            assert_equal [r pexpiretime foo18] $ttl18
        } {} {needs:debug}
    }

    test {All TTL in commands are propagated as absolute timestamp in replication stream} {
        # Make sure that both relative and absolute expire commands are propagated
        # as absolute to replicas for two reasons:
        # 1) We want to avoid replicas retaining data much longer than primary due
        #    to replication lag.
        # 2) We want to unify the way TTLs are replicated in both RDB and replication
        #    stream, which is as absolute timestamps.
        # See: https://github.com/redis/redis/issues/8433

        r flushall ; # Clean up keyspace to avoid interference by keys from other tests
        set repl [attach_to_replication_stream]
        # SET commands
        r set foo1 bar ex 200
        r set foo1 bar px 100000
        r set foo1 bar exat [expr [clock seconds]+100]
        r set foo1 bar pxat [expr [clock milliseconds]+100000]
        r setex foo1 100 bar
        r psetex foo1 100000 bar
        r set foo2 bar
        # EXPIRE-family commands
        r expire foo2 100
        r pexpire foo2 100000
        r set foo3 bar
        r expireat foo3 [expr [clock seconds]+100]
        r pexpireat foo3 [expr [clock seconds]*1000+100000]
        r expireat foo3 [expr [clock seconds]-100]
        # GETEX-family commands
        r set foo4 bar
        r getex foo4 ex 200
        r getex foo4 px 200000
        r getex foo4 exat [expr [clock seconds]+100]
        r getex foo4 pxat [expr [clock milliseconds]+100000]
        # RESTORE commands
        r set foo5 bar
        set encoded [r dump foo5]
        r restore foo6 100000 $encoded
        r restore foo7 [expr [clock milliseconds]+100000] $encoded absttl

        assert_replication_stream $repl {
            {select *}
            {set foo1 bar PXAT *}
            {set foo1 bar PXAT *}
            {set foo1 bar PXAT *}
            {set foo1 bar pxat *}
            {set foo1 bar PXAT *}
            {set foo1 bar PXAT *}
            {set foo2 bar}
            {pexpireat foo2 *}
            {pexpireat foo2 *}
            {set foo3 bar}
            {pexpireat foo3 *}
            {pexpireat foo3 *}
            {del foo3}
            {set foo4 bar}
            {pexpireat foo4 *}
            {pexpireat foo4 *}
            {pexpireat foo4 *}
            {pexpireat foo4 *}
            {set foo5 bar}
            {restore foo6 * * ABSTTL}
            {restore foo7 * * absttl}
        }
        close_replication_stream $repl
    } {} {needs:repl}

    # Start another server to test replication of TTLs
    start_server {tags {needs:repl external:skip}} {
        # Set the outer layer server as primary
        set primary [srv -1 client]
        set primary_host [srv -1 host]
        set primary_port [srv -1 port]
        # Set this inner layer server as replica
        set replica [srv 0 client]

        test {First server should have role slave after REPLICAOF} {
            $replica replicaof $primary_host $primary_port
            wait_for_condition 50 100 {
                [s 0 role] eq {slave}
            } else {
                fail "Replication not started."
            }
        }

        test {For all replicated TTL-related commands, absolute expire times are identical on primary and replica} {
            # Apply each TTL-related command to a unique key on primary
            # SET commands
            $primary set foo1 bar ex 100
            $primary set foo2 bar px 100000
            $primary set foo3 bar exat [expr [clock seconds]+100]
            $primary set foo4 bar pxat [expr [clock milliseconds]+100000]
            $primary setex foo5 100 bar
            $primary psetex foo6 100000 bar
            # EXPIRE-family commands
            $primary set foo7 bar
            $primary expire foo7 100
            $primary set foo8 bar
            $primary pexpire foo8 100000
            $primary set foo9 bar
            $primary expireat foo9 [expr [clock seconds]+100]
            $primary set foo10 bar
            $primary pexpireat foo10 [expr [clock milliseconds]+100000]
            # GETEX commands
            $primary set foo11 bar
            $primary getex foo11 ex 100
            $primary set foo12 bar
            $primary getex foo12 px 100000
            $primary set foo13 bar
            $primary getex foo13 exat [expr [clock seconds]+100]
            $primary set foo14 bar
            $primary getex foo14 pxat [expr [clock milliseconds]+100000]
            # RESTORE commands
            $primary set foo15 bar
            set encoded [$primary dump foo15]
            $primary restore foo16 100000 $encoded
            $primary restore foo17 [expr [clock milliseconds]+100000] $encoded absttl

            # Wait for replica to get the keys and TTLs
            assert {[$primary wait 1 0] == 1}

            # Verify absolute TTLs are identical on primary and replica for all keys
            # This is because TTLs are always replicated as absolute values
            foreach key [$primary keys *] {
                assert_equal [$primary pexpiretime $key] [$replica pexpiretime $key]
            }
        }

        test {expired key which is created in writeable replicas should be deleted by active expiry} {
            $primary flushall
            $replica config set replica-read-only no
            foreach {yes_or_no} {yes no} {
                $replica config set appendonly $yes_or_no
                waitForBgrewriteaof $replica
                set prev_expired [s expired_keys]
                $replica set foo bar PX 1
                wait_for_condition 100 10 {
                    [s expired_keys] eq $prev_expired + 1
                } else {
                    fail "key not expired"
                }
                assert_equal {} [$replica get foo]
            }
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
    } {} {needs:debug}

    test {GETEX use of PERSIST option should remove TTL} {
       r set foo bar EX 100
       r getex foo PERSIST
       r ttl foo
    } {-1}

    test {GETEX use of PERSIST option should remove TTL after loadaof} {
       r config set appendonly yes
       r set foo bar EX 100
       r getex foo PERSIST
       r debug loadaof
       r ttl foo
    } {-1} {needs:debug}

    test {GETEX propagate as to replica as PERSIST, DEL, or nothing} {
        # In the above tests, many keys with random expiration times are set, flush
        # the DBs to avoid active expiry kicking in and messing the replication streams.
        r flushall
       set repl [attach_to_replication_stream]
       r set foo bar EX 100
       r getex foo PERSIST
       r getex foo
       r getex foo exat [expr [clock seconds]-100]
       assert_replication_stream $repl {
           {select *}
           {set foo bar PXAT *}
           {persist foo}
           {del foo}
        }
        close_replication_stream $repl
    } {} {needs:repl}

    test {EXPIRE with NX option on a key with ttl} {
        r SET foo bar EX 100
        assert_equal [r EXPIRE foo 200 NX] 0
        assert_range [r TTL foo] 50 100
    } {}

    test {EXPIRE with NX option on a key without ttl} {
        r SET foo bar
        assert_equal [r EXPIRE foo 200 NX] 1
        assert_range [r TTL foo] 100 200
    } {}

    test {EXPIRE with XX option on a key with ttl} {
        r SET foo bar EX 100
        assert_equal [r EXPIRE foo 200 XX] 1
        assert_range [r TTL foo] 100 200
    } {}

    test {EXPIRE with XX option on a key without ttl} {
        r SET foo bar
        assert_equal [r EXPIRE foo 200 XX] 0
        assert_equal [r TTL foo] -1
    } {}

    test {EXPIRE with GT option on a key with lower ttl} {
        r SET foo bar EX 100
        assert_equal [r EXPIRE foo 200 GT] 1
        assert_range [r TTL foo] 100 200
    } {}

    test {EXPIRE with GT option on a key with higher ttl} {
        r SET foo bar EX 200
        assert_equal [r EXPIRE foo 100 GT] 0
        assert_range [r TTL foo] 100 200
    } {}

    test {EXPIRE with GT option on a key without ttl} {
        r SET foo bar
        assert_equal [r EXPIRE foo 200 GT] 0
        assert_equal [r TTL foo] -1
    } {}

    test {EXPIRE with LT option on a key with higher ttl} {
        r SET foo bar EX 100
        assert_equal [r EXPIRE foo 200 LT] 0
        assert_range [r TTL foo] 50 100
    } {}

    test {EXPIRE with LT option on a key with lower ttl} {
        r SET foo bar EX 200
        assert_equal [r EXPIRE foo 100 LT] 1
        assert_range [r TTL foo] 50 100
    } {}

    test {EXPIRE with LT option on a key without ttl} {
        r SET foo bar
        assert_equal [r EXPIRE foo 100 LT] 1
        assert_range [r TTL foo] 50 100
    } {}

    test {EXPIRE with LT and XX option on a key with ttl} {
        r SET foo bar EX 200
        assert_equal [r EXPIRE foo 100 LT XX] 1
        assert_range [r TTL foo] 50 100
    } {}

    test {EXPIRE with LT and XX option on a key without ttl} {
        r SET foo bar
        assert_equal [r EXPIRE foo 200 LT XX] 0
        assert_equal [r TTL foo] -1
    } {}

    test {EXPIRE with conflicting options: LT GT} {
        catch {r EXPIRE foo 200 LT GT} e
        set e
    } {ERR GT and LT options at the same time are not compatible}

    test {EXPIRE with conflicting options: NX GT} {
        catch {r EXPIRE foo 200 NX GT} e
        set e
    } {ERR NX and XX, GT or LT options at the same time are not compatible}

    test {EXPIRE with conflicting options: NX LT} {
        catch {r EXPIRE foo 200 NX LT} e
        set e
    } {ERR NX and XX, GT or LT options at the same time are not compatible}

    test {EXPIRE with conflicting options: NX XX} {
        catch {r EXPIRE foo 200 NX XX} e
        set e
    } {ERR NX and XX, GT or LT options at the same time are not compatible}

    test {EXPIRE with unsupported options} {
        catch {r EXPIRE foo 200 AB} e
        set e
    } {ERR Unsupported option AB}

    test {EXPIRE with unsupported options} {
        catch {r EXPIRE foo 200 XX AB} e
        set e
    } {ERR Unsupported option AB}

    test {EXPIRE with negative expiry} {
        r SET foo bar EX 100
        assert_equal [r EXPIRE foo -10 LT] 1
        assert_equal [r TTL foo] -2
    } {}

    test {EXPIRE with negative expiry on a non-valitale key} {
        r SET foo bar
        assert_equal [r EXPIRE foo -10 LT] 1
        assert_equal [r TTL foo] -2
    } {}

    test {EXPIRE with non-existed key} {
        assert_equal [r EXPIRE none 100 NX] 0
        assert_equal [r EXPIRE none 100 XX] 0
        assert_equal [r EXPIRE none 100 GT] 0
        assert_equal [r EXPIRE none 100 LT] 0
    } {}

    test {Redis should not propagate the read command on lazy expire} {
        r debug set-active-expire 0
        r flushall ; # Clean up keyspace to avoid interference by keys from other tests
        r set foo bar PX 1
        set repl [attach_to_replication_stream]
        wait_for_condition 50 100 {
            [r get foo] eq {}
        } else {
            fail "Replication not started."
        }

        # dummy command to verify nothing else gets into the replication stream.
        r set x 1

        assert_replication_stream $repl {
            {select *}
            {del foo}
            {set x 1}
        }
        close_replication_stream $repl
        assert_equal [r debug set-active-expire 1] {OK}
    } {} {needs:debug}

    test {SCAN: Lazy-expire should not be wrapped in MULTI/EXEC} {
        r debug set-active-expire 0
        r flushall

        r set foo1 bar PX 1
        r set foo2 bar PX 1
        after 2

        set repl [attach_to_replication_stream]

        r scan 0

        assert_replication_stream $repl {
            {select *}
            {del foo*}
            {del foo*}
        }
        close_replication_stream $repl
        assert_equal [r debug set-active-expire 1] {OK}
    } {} {needs:debug}

    test {RANDOMKEY: Lazy-expire should not be wrapped in MULTI/EXEC} {
        r debug set-active-expire 0
        r flushall

        r set foo1 bar PX 1
        r set foo2 bar PX 1
        after 2

        set repl [attach_to_replication_stream]

        r randomkey

        assert_replication_stream $repl {
            {select *}
            {del foo*}
            {del foo*}
        }
        close_replication_stream $repl
        assert_equal [r debug set-active-expire 1] {OK}
    } {} {needs:debug}
}

start_cluster 1 0 {tags {"expire external:skip cluster"}} {
    test "expire scan should skip dictionaries with lot's of empty buckets" {
        r debug set-active-expire 0

        # Collect two slots to help determine the expiry scan logic is able
        # to go past certain slots which aren't valid for scanning at the given point of time.
        # And the next non empty slot after that still gets scanned and expiration happens.

        # hashslot(alice) is 749
        r psetex alice 500 val

        # hashslot(foo) is 12182
        # fill data across different slots with expiration
        for {set j 1} {$j <= 100} {incr j} {
            r psetex "{foo}$j" 500 a
        }
        # hashslot(key) is 12539
        r psetex key 500 val

        # disable resizing, the reason for not using slow bgsave is because
        # it will hit the dict_force_resize_ratio.
        r debug dict-resizing 0

        # delete data to have lot's (99%) of empty buckets (slot 12182 should be skipped)
        for {set j 1} {$j <= 99} {incr j} {
            r del "{foo}$j"
        }

        # Trigger a full traversal of all dictionaries.
        r keys *

        r debug set-active-expire 1

        # Verify {foo}100 still exists and remaining got cleaned up
        wait_for_condition 20 100 {
            [r dbsize] eq 1
        } else {
            if {[r dbsize] eq 0} {
                puts [r debug htstats 0]
                fail "scan didn't handle slot skipping logic."
            } else {
                puts [r debug htstats 0]
                fail "scan didn't process all valid slots."
            }
        }

        # Enable resizing
        r debug dict-resizing 1

        # put some data into slot 12182 and trigger the resize
        r psetex "{foo}0" 500 a

        # Verify all keys have expired
        wait_for_condition 400 100 {
            [r dbsize] eq 0
        } else {
            puts [r dbsize]
            flush stdout
            fail "Keys did not actively expire."
        }

        # Make sure we don't have any timeouts.
        assert_equal 0 [s 0 expired_time_cap_reached_count]
    } {} {needs:debug}
}
