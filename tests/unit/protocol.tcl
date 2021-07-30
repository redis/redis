start_server {tags {"protocol network"}} {
    test "Handle an empty query" {
        reconnect
        r write "\r\n"
        r flush
        assert_equal "PONG" [r ping]
    }

    test "Negative multibulk length" {
        reconnect
        r write "*-10\r\n"
        r flush
        assert_equal PONG [r ping]
    }

    test "Out of range multibulk length" {
        reconnect
        r write "*20000000\r\n"
        r flush
        assert_error "*invalid multibulk length*" {r read}
    }

    test "Wrong multibulk payload header" {
        reconnect
        r write "*3\r\n\$3\r\nSET\r\n\$1\r\nx\r\nfooz\r\n"
        r flush
        assert_error "*expected '$', got 'f'*" {r read}
    }

    test "Negative multibulk payload length" {
        reconnect
        r write "*3\r\n\$3\r\nSET\r\n\$1\r\nx\r\n\$-10\r\n"
        r flush
        assert_error "*invalid bulk length*" {r read}
    }

    test "Out of range multibulk payload length" {
        reconnect
        r write "*3\r\n\$3\r\nSET\r\n\$1\r\nx\r\n\$2000000000\r\n"
        r flush
        assert_error "*invalid bulk length*" {r read}
    }

    test "Non-number multibulk payload length" {
        reconnect
        r write "*3\r\n\$3\r\nSET\r\n\$1\r\nx\r\n\$blabla\r\n"
        r flush
        assert_error "*invalid bulk length*" {r read}
    }

    test "Multi bulk request not followed by bulk arguments" {
        reconnect
        r write "*1\r\nfoo\r\n"
        r flush
        assert_error "*expected '$', got 'f'*" {r read}
    }

    test "Generic wrong number of args" {
        reconnect
        assert_error "*wrong*arguments*ping*" {r ping x y z}
    }

    test "Unbalanced number of quotes" {
        reconnect
        r write "set \"\"\"test-key\"\"\" test-value\r\n"
        r write "ping\r\n"
        r flush
        assert_error "*unbalanced*" {r read}
    }

    set c 0
    foreach seq [list "\x00" "*\x00" "$\x00"] {
        incr c
        test "Protocol desync regression test #$c" {
            if {$::tls} {
                set s [::tls::socket [srv 0 host] [srv 0 port]]
            } else {
                set s [socket [srv 0 host] [srv 0 port]]
            }
            puts -nonewline $s $seq
            set payload [string repeat A 1024]"\n"
            set test_start [clock seconds]
            set test_time_limit 30
            while 1 {
                if {[catch {
                    puts -nonewline $s payload
                    flush $s
                    incr payload_size [string length $payload]
                }]} {
                    set retval [gets $s]
                    close $s
                    break
                } else {
                    set elapsed [expr {[clock seconds]-$test_start}]
                    if {$elapsed > $test_time_limit} {
                        close $s
                        error "assertion:Redis did not closed connection after protocol desync"
                    }
                }
            }
            set retval
        } {*Protocol error*}
    }
    unset c

    # recover the broken connection
    reconnect
    r ping

    # raw RESP response tests
    r readraw 1

    test "raw protocol response" {
        r srandmember nonexisting_key
    } {*-1}

    r deferred 1

    test "raw protocol response - deferred" {
        r srandmember nonexisting_key
        r read
    } {*-1}

    test "raw protocol response - multiline" {
        r sadd ss a
        assert_equal [r read] {:1}
        r srandmember ss 100
        assert_equal [r read] {*1}
        assert_equal [r read] {$1}
        assert_equal [r read] {a}
    }

    # restore connection settings
    r readraw 0
    r deferred 0

    # check the connection still works
    assert_equal [r ping] {PONG}

    test {RESP3 attributes} {
        r hello 3
        set res [r debug protocol attrib]
        # currently the parser in redis.tcl ignores the attributes

        # restore state
        r hello 2
        set _ $res
    } {Some real reply following the attribute} {resp3}

    test {RESP3 attributes readraw} {
        r hello 3
        r readraw 1
        r deferred 1

        r debug protocol attrib
        assert_equal [r read] {|1}
        assert_equal [r read] {$14}
        assert_equal [r read] {key-popularity}
        assert_equal [r read] {*2}
        assert_equal [r read] {$7}
        assert_equal [r read] {key:123}
        assert_equal [r read] {:90}
        assert_equal [r read] {$39}
        assert_equal [r read] {Some real reply following the attribute}

        # restore state
        r readraw 0
        r deferred 0
        r hello 2
        set _ {}
    } {} {resp3}

    test {RESP3 attributes on RESP2} {
        r hello 2
        set res [r debug protocol attrib]
        set _ $res
    } {Some real reply following the attribute}

    test "test big number parsing" {
        r hello 3
        r debug protocol bignum
    } {1234567999999999999999999999999999999} {needs:debug resp3}

    test "test bool parsing" {
        r hello 3
        assert_equal [r debug protocol true] 1
        assert_equal [r debug protocol false] 0
        r hello 2
        assert_equal [r debug protocol true] 1
        assert_equal [r debug protocol false] 0
        set _ {}
    } {} {needs:debug resp3}
}

start_server {tags {"regression"}} {
    test "Regression for a crash with blocking ops and pipelining" {
        set rd [redis_deferring_client]
        set fd [r channel]
        set proto "*3\r\n\$5\r\nBLPOP\r\n\$6\r\nnolist\r\n\$1\r\n0\r\n"
        puts -nonewline $fd $proto$proto
        flush $fd
        set res {}

        $rd rpush nolist a
        $rd read
        $rd rpush nolist a
        $rd read
    }
}
