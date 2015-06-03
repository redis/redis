start_server {tags {"protocol"}} {
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
            set s [socket [srv 0 host] [srv 0 port]]
            # windows - set nonblocking
            fconfigure $s -blocking false
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
                    #windows - don't read after reset
                    set retval [gets $s]
                    close $s
                   break
                } else {
                    #windows - if data available, read line
                    if {[read $s 1] ne ""}  {
						set retval [gets $s]
						if {[string match {*Protocol error*} $retval]} { break }
					}
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
