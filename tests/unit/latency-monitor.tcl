start_server {tags {"latency-monitor needs:latency"}} {
    # Set a threshold high enough to avoid spurious latency events.
    r config set latency-monitor-threshold 200
    r latency reset

    test {LATENCY HISTOGRAM with empty histogram} {
        r config resetstat
        set histo [dict create {*}[r latency histogram]]
        # Config resetstat is recorded
        assert_equal [dict size $histo] 1
        assert_match {*config|resetstat*} $histo
    }

    test {LATENCY HISTOGRAM all commands} {
        r config resetstat
        r set a b
        r set c d
        set histo [dict create {*}[r latency histogram]]
        assert_match {calls 2 histogram_usec *} [dict get $histo set]
        assert_match {calls 1 histogram_usec *} [dict get $histo "config|resetstat"]
    }

    test {LATENCY HISTOGRAM sub commands} {
        r config resetstat
        r client id
        r client list
        # parent command reply with its sub commands
        set histo [dict create {*}[r latency histogram client]]
        assert {[dict size $histo] == 2}
        assert_match {calls 1 histogram_usec *} [dict get $histo "client|id"]
        assert_match {calls 1 histogram_usec *} [dict get $histo "client|list"]

        # explicitly ask for one sub-command
        set histo [dict create {*}[r latency histogram "client|id"]]
        assert {[dict size $histo] == 1}
        assert_match {calls 1 histogram_usec *} [dict get $histo "client|id"]
    }

    test {LATENCY HISTOGRAM with a subset of commands} {
        r config resetstat
        r set a b
        r set c d
        r get a
        r hset f k v
        r hgetall f
        set histo [dict create {*}[r latency histogram set hset]]
        assert_match {calls 2 histogram_usec *} [dict get $histo set]
        assert_match {calls 1 histogram_usec *} [dict get $histo hset]
        assert_equal [dict size $histo] 2
        set histo [dict create {*}[r latency histogram hgetall get zadd]]
        assert_match {calls 1 histogram_usec *} [dict get $histo hgetall]
        assert_match {calls 1 histogram_usec *} [dict get $histo get]
        assert_equal [dict size $histo] 2
    }

    test {LATENCY HISTOGRAM command} {
        r config resetstat
        r set a b
        r get a
        assert {[llength [r latency histogram set get]] == 4}
    }

    test {LATENCY HISTOGRAM with wrong command name skips the invalid one} {
        r config resetstat
        assert {[llength [r latency histogram blabla]] == 0}
        assert {[llength [r latency histogram blabla blabla2 set get]] == 0}
        r set a b
        r get a
        assert_match {calls 1 histogram_usec *} [lindex [r latency histogram blabla blabla2 set get] 1]
        assert_match {calls 1 histogram_usec *} [lindex [r latency histogram blabla blabla2 set get] 3]
        assert {[string length [r latency histogram blabla set get]] > 0}
    }

tags {"needs:debug"} {
    test {Test latency events logging} {
        r debug sleep 0.3
        after 1100
        r debug sleep 0.4
        after 1100
        r debug sleep 0.5
        assert {[r latency history command] >= 3}
    }

    test {LATENCY HISTORY output is ok} {
        set min 250
        set max 450
        foreach event [r latency history command] {
            lassign $event time latency
            if {!$::no_latency} {
                assert {$latency >= $min && $latency <= $max}
            }
            incr min 100
            incr max 100
            set last_time $time ; # Used in the next test
        }
    }

    test {LATENCY LATEST output is ok} {
        foreach event [r latency latest] {
            lassign $event eventname time latency max
            assert {$eventname eq "command"}
            if {!$::no_latency} {
                assert {$max >= 450 & $max <= 650}
                assert {$time == $last_time}
            }
            break
        }
    }

    test {LATENCY GRAPH can output the event graph} {
        set res [r latency graph command]
        assert_match {*command*high*low*} $res

        # These numbers are taken from the "Test latency events logging" test.
        # (debug sleep 0.3) and (debug sleep 0.5), using range to prevent timing issue.
        regexp "command - high (.*?) ms, low (.*?) ms" $res -> high low
        assert_morethan_equal $high 500
        assert_morethan_equal $low 300
    }
} ;# tag

    test {LATENCY of expire events are correctly collected} {
        r config set latency-monitor-threshold 20
        r flushdb
        if {$::valgrind} {set count 100000} else {set count 1000000}
        r eval {
            local i = 0
            while (i < tonumber(ARGV[1])) do
                redis.call('sadd',KEYS[1],i)
                i = i+1
             end
        } 1 mybigkey $count
        r pexpire mybigkey 50
        wait_for_condition 5 100 {
            [r dbsize] == 0
        } else {
            fail "key wasn't expired"
        }
        assert_match {*expire-cycle*} [r latency latest]

        test {LATENCY GRAPH can output the expire event graph} {
             assert_match {*expire-cycle*high*low*} [r latency graph expire-cycle]
        }

        r config set latency-monitor-threshold 200
    }

    test {LATENCY HISTORY / RESET with wrong event name is fine} {
        assert {[llength [r latency history blabla]] == 0}
        assert {[r latency reset blabla] == 0}
    }

    test {LATENCY DOCTOR produces some output} {
        assert {[string length [r latency doctor]] > 0}
    }

    test {LATENCY RESET is able to reset events} {
        assert {[r latency reset] > 0}
        assert {[r latency latest] eq {}}
    }

    test {LATENCY HELP should not have unexpected options} {
        catch {r LATENCY help xxx} e
        assert_match "*wrong number of arguments for 'latency|help' command" $e
    }
}
