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
        r write "*3000000000\r\n"
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

    # The parser keeps a partially parsed command around between reads. A
    # protocol error detected on a later read used to be recorded on that
    # carried over command but never reported, leaving the client hanging
    # while its query buffer kept growing. Send the header and the offending
    # part in two separate writes so they land in two different reads.
    test "Protocol error is reported when the command is split across reads" {
        reconnect
        r write "*3\r\n\$3\r\nSET\r\n\$1\r\nx\r\n"
        r flush
        after 100
        r write "fooz\r\n"
        r flush
        assert_error "*expected '$', got 'f'*" {r read}
    }

    test "Invalid bulk length is reported when the command is split across reads" {
        reconnect
        r write "*3\r\n\$3\r\nSET\r\n\$1\r\nx\r\n"
        r flush
        after 100
        r write "\$blabla\r\n"
        r flush
        assert_error "*invalid bulk length*" {r read}
    }

    # PROTO_INLINE_MAX_SIZE is 64k. A bulk count string longer than that has to
    # be rejected instead of being buffered up to the query buffer limit. The
    # header necessarily spans several reads, so this only works once an error
    # found on a later read is actually reported.
    test "Too big bulk count string" {
        reconnect
        r write "*1\r\n\$"
        r flush
        r write [string repeat 1 [expr {64*1024+16}]]
        r flush
        assert_error "*too big bulk count string*" {r read}
    }

    test "Too big multibulk count string" {
        reconnect
        r write "*"
        r flush
        r write [string repeat 1 [expr {64*1024+16}]]
        r flush
        assert_error "*too big mbulk count string*" {r read}
    }

    test "Too big inline request" {
        reconnect
        r write [string repeat A [expr {64*1024+16}]]
        r flush
        assert_error "*too big inline request*" {r read}
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

    set nullres {*-1}
    if {$::force_resp3} {
        set nullres {_}
    }

    test "raw protocol response" {
        r srandmember nonexisting_key
    } "$nullres"

    r deferred 1

    test "raw protocol response - deferred" {
        r srandmember nonexisting_key
        r read
    } "$nullres"

    test "raw protocol response - multiline" {
        r sadd ss a
        assert_equal [r read] {:1}
        r srandmember ss 100
        assert_equal [r read] {*1}
        assert_equal [r read] {$1}
        assert_equal [r read] {a}
    }

    test "bulk reply protocol" {
        # value=2 (int encoding)
        r set crlf 2
        assert_equal [r rawread 5] "+OK\r\n"
        r get crlf
        assert_equal [r rawread 7] "\$1\r\n2\r\n"

        # value=2147483647 (int encoding)
        r set crlf 2147483647
        assert_equal [r rawread 5] "+OK\r\n"
        r get crlf
        assert_equal [r rawread 17] "\$10\r\n2147483647\r\n"

        # value=-2147483648 (int encoding)
        r set crlf -2147483648
        assert_equal [r rawread 5] "+OK\r\n"
        r get crlf
        assert_equal [r rawread 18] "\$11\r\n-2147483648\r\n"

        # value=-9223372036854775809 (embstr encoding)
        r set crlf -9223372036854775809
        assert_equal [r rawread 5] "+OK\r\n"
        r get crlf
        assert_equal [r rawread 27] "\$20\r\n-9223372036854775809\r\n"

        # value=9223372036854775808 (embstr encoding)
        r set crlf 9223372036854775808
        assert_equal [r rawread 5] "+OK\r\n"
        r get crlf
        assert_equal [r rawread 26] "\$19\r\n9223372036854775808\r\n"

        # normal sds (embstr encoding)
        r set crlf aaaaaaaaaaaaaaaa
        assert_equal [r rawread 5] "+OK\r\n"
        r get crlf
        assert_equal [r rawread 23] "\$16\r\naaaaaaaaaaaaaaaa\r\n"

        # normal sds (raw string encoding) with 45 'a'
        set rawstr [string repeat "a" 45]
        r set crlf $rawstr
        assert_equal [r rawread 5] "+OK\r\n"
        r get crlf
        assert_equal [r rawread 52] "\$45\r\n$rawstr\r\n"

        r del crlf
        assert_equal [r rawread 4] ":1\r\n"
    }

    # restore connection settings
    r readraw 0
    r deferred 0

    # check the connection still works
    assert_equal [r ping] {PONG}

    test {RESP3 attributes} {
        r hello 3
        assert_equal {Some real reply following the attribute} [r debug protocol attrib]
        assert_equal {key-popularity {key:123 90}} [r attributes]

        # make sure attributes are not kept from previous command
        r ping
        assert_error {*attributes* no such element in array} {r attributes}

        # restore state
        r hello 2
        set _ ""
    } {} {needs:debug resp3}

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
    } {} {needs:debug resp3}

    test {RESP3 attributes on RESP2} {
        r hello 2
        set res [r debug protocol attrib]
        set _ $res
    } {Some real reply following the attribute} {needs:debug}

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

    test "test verbatim str parsing" {
        r hello 3
        r debug protocol verbatim
    } "This is a verbatim\nstring" {needs:debug resp3}

    test "test large number of args" {
        r flushdb
        set args [split [string trim [string repeat "k v " 10000]]]
        lappend args "{k}2" v2
        r mset {*}$args
        assert_equal [r get "{k}2"] v2
    }
    
    test "test argument rewriting - issue 9598" {
        # INCRBYFLOAT uses argument rewriting for correct float value propagation.
        # We use it to make sure argument rewriting works properly. It's important 
        # this test is run under valgrind to verify there are no memory leaks in 
        # arg buffer handling.
        r flushdb

        # Test normal argument handling
        r set k 0
        assert_equal [r incrbyfloat k 1.0] 1
        
        # Test argument handing in multi-state buffers
        r multi
        r incrbyfloat k 1.0
        assert_equal [r exec] 2
    }

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
        $rd close
    }
}

start_server {tags {"regression"}} {
    test "Regression for a crash with cron release of client arguments" {
        r write "*3\r\n"
        r flush
        after 3000 ;# wait for c->argv to be released due to timeout
        r write "\$3\r\nSET\r\n\$3\r\nkey\r\n\$1\r\n0\r\n"
        r flush
        r read
    } {OK}
}

start_server {tags {"protocol network"}} {
    # Pooled pending commands keep their unshared EMBSTR arguments and the next
    # command parsed into them overwrites those objects in place.
    foreach lookahead {16 256 1} {
        test "Pipelined arguments of changing sizes are parsed correctly (lookahead $lookahead)" {
            r config set lookahead $lookahead
            # Sizes cross the EMBSTR allocation classes and the EMBSTR limit.
            set sizes {0 1 12 13 28 29 44 45 60 3 44 0 20}
            set proto ""
            set expected {}
            for {set i 0} {$i < 300} {incr i} {
                set len [lindex $sizes [expr {$i % [llength $sizes]}]]
                set val [string repeat [format %c [expr {97 + $i % 26}]] $len]
                set key "key:[expr {$i % 7}]:[string repeat x [expr {$i % 40}]]"
                append proto [formatCommand set $key $val]
                append proto [formatCommand get $key]
                append proto [formatCommand echo $val]
                lappend expected OK $val $val
                if {$i % 10 == 0} {
                    append proto "PING\r\n"
                    lappend expected PONG
                }
            }
            r write $proto
            r flush
            set got {}
            foreach _ $expected {lappend got [r read]}
            assert_equal $expected $got
        }
    }
    r config set lookahead 16

    test "Arguments retained by a command are not overwritten by later commands" {
        set rd [redis_deferring_client]
        $rd subscribe chan:aaaa chan:bbbb
        $rd read
        $rd read
        r client setname name:cccc
        # Arguments of the same length would be written over the retained ones
        # if they had been recycled.
        for {set i 0} {$i < 20} {incr i} {
            r set name:dddd name:eeee
            r echo zzzz:zzzz
        }
        assert_equal name:cccc [r client getname]
        assert_equal {chan:aaaa chan:bbbb} [lsort [r pubsub channels chan:*]]
        r publish chan:aaaa hello
        assert_equal {message chan:aaaa hello} [$rd read]
        $rd close
    }

    test "Wide pipelined commands are parsed correctly and keep what they retain" {
        # Commands with 64 arguments or more are pooled too, with their
        # arguments recycled. 600 keys is past the size of a pooled argv array.
        foreach nkeys {40 70 200 600} {
            set proto ""
            set expected {}
            for {set i 0} {$i < 12} {incr i} {
                set kv {}
                set keys {}
                set vals {}
                for {set j 0} {$j < $nkeys} {incr j} {
                    # The same lengths in every round, with different bytes.
                    set val [format "%04d:%04d" $i $j]
                    lappend kv "wide:$j" $val
                    lappend keys "wide:$j"
                    lappend vals $val
                }
                append proto [formatCommand mset {*}$kv]
                append proto [formatCommand exists {*}$keys]
                append proto [formatCommand mget {*}$keys]
                lappend expected OK $nkeys $vals
            }
            r write $proto
            r flush
            set got {}
            foreach _ $expected {lappend got [r read]}
            assert_equal $expected $got
        }
    }

    test "Command split inside its multibulk and bulk length lines" {
        r write "*"
        r flush
        after 100
        r write "3\r\n\$3\r\nSET\r\n\$3\r\nkey\r\n\$"
        r flush
        after 100
        r write "5\r\nvalue\r\n"
        r flush
        assert_equal OK [r read]
        assert_equal value [r get key]
    }
}
