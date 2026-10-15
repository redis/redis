# BITGETINTARRAY / BITRANGEINTARRAY follow redis-roaring R.GETINTARRAY /
# R.RANGEINTARRAY at
# e7382b7cea1ec495304e8aa60916c49673e74c1d. Ranges address inclusive ranks in
# the sorted set-bit offsets, while native bitmap offsets may exceed 32 bits.
proc intarray_seed {key bits} {
    set old [lindex [r config get bitmap-default-roaring] 1]
    r del $key
    if {[llength $bits] == 0} {
        r restore $key 0 [empty_roaring_bitmap_dump_payload] replace
        return
    }
    r config set bitmap-default-roaring yes
    set code [catch {
        foreach bit $bits {r setbit $key $bit 1}
    } result opts]
    r config set bitmap-default-roaring $old
    if {$code != 0} {return -options $opts $result}
}

proc intarray_random {state_name modulus} {
    upvar 1 $state_name state
    set state [expr {($state * 1103515245 + 12345) & 0x7fffffff}]
    return [expr {$state % $modulus}]
}

start_server {tags {"bitmap" "bitmap-roaring" "cluster:skip"}} {
    test {Integer-array commands enforce their arity} {
        foreach args {{bitgetintarray} {bitgetintarray key extra}
                      {bitrangeintarray} {bitrangeintarray key}
                      {bitrangeintarray key 0} {bitrangeintarray key 0 1 extra}} {
            assert_error {ERR wrong number of arguments*} {r {*}$args}
        }
    }

    test {BITGETINTARRAY returns sorted unique offsets independently of creation config} {
        intarray_seed intarray:basic {65536 9 1 0 9 257 2 65535}
        foreach config {no yes} {
            r config set bitmap-default-roaring $config
            assert_equal {0 1 2 9 257 65535 65536} [r bitgetintarray intarray:basic]
            assert_equal {1 2 9} [r bitrangeintarray intarray:basic 1 3]
            assert_equal bitmap [r type intarray:basic]
        }
        r config set bitmap-default-roaring no
    }

    test {BITRANGEINTARRAY uses inclusive ranks rather than bit offsets} {
        intarray_seed intarray:ranks {1 2 3 5 6 7 8 9 10}
        assert_equal {1 2 3 5 6 7} [r bitrangeintarray intarray:ranks 0 5]
        assert_equal {7 8 9} [r bitrangeintarray intarray:ranks 5 7]
        assert_equal {1} [r bitrangeintarray intarray:ranks 0 0]
        assert_equal {10} [r bitrangeintarray intarray:ranks 8 8]
        assert_equal {9 10} [r bitrangeintarray intarray:ranks 7 99]
        assert_equal {} [r bitrangeintarray intarray:ranks 9 99]
        assert_equal {} [r bitrangeintarray intarray:ranks 8 7]
        assert_equal {} [r bitrangeintarray intarray:ranks 4294967295 4294967295]
        assert_equal {} [r bitrangeintarray intarray:ranks 4294967295 0]
    }

    test {Integer-array commands return empty arrays for missing and empty native keys} {
        r del intarray:missing
        intarray_seed intarray:empty {}
        intarray_seed intarray:zero {8192}
        r setbit intarray:zero 8192 0
        foreach key {intarray:missing intarray:empty intarray:zero} {
            assert_equal {} [r bitgetintarray $key]
            assert_equal {} [r bitrangeintarray $key 0 0]
            assert_equal {} [r bitrangeintarray $key 0 99999999]
            assert_equal {} [r bitrangeintarray $key 1 0]
        }
        assert_equal 0 [r exists intarray:missing]
        assert_equal bitmap [r type intarray:empty]
        assert_equal bitmap [r type intarray:zero]
    }

    test {Integer-array commands reject strings and other types without conversion} {
        r set intarray:string [binary format H* 8040]
        r set intarray:integer 123
        r set intarray:emptystring {}
        r lpush intarray:list value
        foreach config {no yes} {
            r config set bitmap-default-roaring $config
            foreach key {intarray:string intarray:integer intarray:emptystring intarray:list} {
                assert_error {WRONGTYPE Operation against a key holding the wrong kind of value} {
                    r bitgetintarray $key
                }
                assert_error {WRONGTYPE Operation against a key holding the wrong kind of value} {
                    r bitrangeintarray $key 0 0
                }
                # The module checks type before either argument or the range cap.
                foreach bounds {{invalid invalid} {0 4294967295} {1 0}} {
                    assert_error {WRONGTYPE Operation against a key holding the wrong kind of value} {
                        r bitrangeintarray $key {*}$bounds
                    }
                }
            }
        }
        r config set bitmap-default-roaring no
        assert_equal [binary format H* 8040] [r get intarray:string]
        assert_equal string [r type intarray:integer]
        assert_equal {} [r get intarray:emptystring]
        assert_equal {value} [r lrange intarray:list 0 -1]
    }

    test {BITRANGEINTARRAY validates unsigned 32-bit start before end, even for missing keys} {
        intarray_seed intarray:parse {0 9}
        set invalid_ranks [list {} abc -1 -0 +1 01 { 1} {1 } 1.5 1e1 0x1 \
            4294967296 9223372036854775807 9223372036854775808]
        foreach key {intarray:parse intarray:missing intarray:empty} {
            foreach invalid $invalid_ranks {
                assert_error {ERR invalid start: must be an unsigned 32 bit integer} {
                    r bitrangeintarray $key $invalid invalid
                }
                assert_error {ERR invalid end: must be an unsigned 32 bit integer} {
                    r bitrangeintarray $key 0 $invalid
                }
                # An invalid end is not hidden by a start beyond cardinality.
                assert_error {ERR invalid end: must be an unsigned 32 bit integer} {
                    r bitrangeintarray $key 4294967295 $invalid
                }
            }
        }
    }

    test {BITRANGEINTARRAY caps the requested inclusive span before truncation or missing-key reply} {
        intarray_seed intarray:cap {4 99}
        foreach key {intarray:cap intarray:missing intarray:empty} {
            foreach bounds {{0 100000000} {1 100000001} {0 4294967295}
                            {4194967295 4294967295}} {
                assert_error {Roaring: range too large: maximum 100000000 elements} {
                    r bitrangeintarray $key {*}$bounds
                }
            }
            assert_equal {} [r bitrangeintarray $key 4194967296 4294967295]
            assert_equal {} [r bitrangeintarray $key 4294967295 0]
        }
        assert_equal {4 99} [r bitrangeintarray intarray:cap 0 99999999]
        assert_equal {99} [r bitrangeintarray intarray:cap 1 100000000]
    }

    test {Integer-array replies contain RESP integers under RESP2 and RESP3} {
        intarray_seed intarray:protocol {0 8 65536}
        foreach protocol {2 3} {
            set client [redis_client]
            $client hello $protocol
            $client readraw 1
            assert_equal {*3} [$client bitgetintarray intarray:protocol]
            foreach bit {0 8 65536} {assert_equal ":$bit" [$client read]}
            assert_equal {*2} [$client bitrangeintarray intarray:protocol 1 9]
            foreach bit {8 65536} {assert_equal ":$bit" [$client read]}
            assert_equal {*0} [$client bitgetintarray intarray:missing]
            assert_equal {*0} [$client bitrangeintarray intarray:empty 0 1]
            $client close
        }
    }

    test {Integer-array commands respect CLIENT REPLY OFF and SKIP} {
        intarray_seed intarray:suppressed {0 8 65536}
        set client [redis_deferring_client]
        $client client reply off
        $client bitgetintarray intarray:suppressed
        $client bitrangeintarray intarray:suppressed 1 9
        $client client reply on
        assert_equal OK [$client read]
        foreach args {{bitgetintarray intarray:suppressed}
                      {bitrangeintarray intarray:suppressed 1 9}} {
            $client client reply skip
            $client {*}$args
            $client ping
            assert_equal PONG [$client read]
        }
        $client close
    }

    test {Integer-array traversal crosses 256-value batches and seeks across containers} {
        set expected {}
        for {set i 0} {$i < 1100} {incr i} {
            lappend expected [expr {$i * 65536 + ($i % 17)}]
        }
        intarray_seed intarray:containers $expected
        assert_equal $expected [r bitgetintarray intarray:containers]
        foreach bounds {{0 255} {0 256} {255 256} {256 512} {257 513}
                        {510 1025} {900 1099} {1099 1200} {1100 1200}} {
            lassign $bounds start end
            assert_equal [lrange $expected $start $end] \
                [r bitrangeintarray intarray:containers $start $end]
        }
    }

    test {Integer-array traversal handles dense bitset and run containers} {
        set bitset {}
        for {set i 0} {$i < 10000} {incr i 2} {lappend bitset $i}
        intarray_seed intarray:bitset $bitset
        assert_equal $bitset [r bitgetintarray intarray:bitset]
        assert_equal [lrange $bitset 3999 4600] \
            [r bitrangeintarray intarray:bitset 3999 4600]

        set old [config_get_set bitmap-default-roaring yes]
        r del intarray:run
        r setbit intarray:run 8191 0
        r bitop not intarray:run intarray:run
        r config set bitmap-default-roaring $old
        set run {}
        for {set i 0} {$i < 8192} {incr i} {lappend run $i}
        assert_equal $run [r bitgetintarray intarray:run]
        assert_equal [lrange $run 4093 4609] [r bitrangeintarray intarray:run 4093 4609]
        assert_equal {8190 8191} [r bitrangeintarray intarray:run 8190 9000]
    }

    test {Integer-array commands agree with a deterministic sorted-set oracle after mutations} {
        set state 15331
        for {set trial 0} {$trial < 30} {incr trial} {
            set bits {}
            for {set i 0} {$i < 90} {incr i} {
                set container [intarray_random state 12]
                set offset [intarray_random state 65536]
                lappend bits [expr {$container * 65536 + $offset}]
                if {$i % 7 == 0} {lappend bits [lindex $bits end]}
            }
            intarray_seed intarray:oracle $bits
            set present [dict create]
            foreach bit $bits {dict set present $bit 1}
            for {set i 0} {$i < [llength $bits]} {incr i 4} {
                set bit [lindex $bits $i]
                r setbit intarray:oracle $bit 0
                dict unset present $bit
            }
            set expected [lsort -integer [dict keys $present]]
            assert_equal $expected [r bitgetintarray intarray:oracle]
            assert_equal [llength $expected] [r bitcount intarray:oracle]
            for {set query 0} {$query < 15} {incr query} {
                set start [intarray_random state 110]
                set end [intarray_random state 110]
                assert_equal [lrange $expected $start $end] \
                    [r bitrangeintarray intarray:oracle $start $end]
            }
        }
    }

    if {[s arch_bits] == 64} {
        test {Integer-array commands preserve 64-bit offsets up to the native bitmap limit} {
            set max_bytes [expr {(1 << 60) - 1}]
            set max_bit [expr {$max_bytes * 8 - 1}]
            set old_limit [config_get_set proto-max-bulk-len $max_bytes]
            set expected [list 0 4294967295 4294967296 1099511627776 $max_bit]
            intarray_seed intarray:wide $expected
            # Reading stored offsets must not reapply the current write limit.
            r config set proto-max-bulk-len 1048576
            assert_equal $expected [r bitgetintarray intarray:wide]
            assert_equal [lrange $expected 1 4] [r bitrangeintarray intarray:wide 1 4]
            assert_equal [list $max_bit] [r bitrangeintarray intarray:wide 4 99]
            r config set proto-max-bulk-len $old_limit
            r del intarray:wide
        }
    }

    test {Integer-array command metadata declares one read-only key} {
        foreach command {bitgetintarray bitrangeintarray} {
            set info [lindex [r command info $command] 0]
            assert {[lsearch -exact [lindex $info 2] readonly] >= 0}
            assert {[lsearch -exact [lindex $info 2] write] < 0}
        }
        assert_equal {{intarray:metadata {RO access}}} \
            [r command getkeysandflags bitgetintarray intarray:metadata]
        assert_equal {{intarray:metadata {RO access}}} \
            [r command getkeysandflags bitrangeintarray intarray:metadata 0 9]
    }

    test {Integer-array commands work in read-only scripts} {
        intarray_seed intarray:script {2 5 11 65536}
        assert_equal {2 5 11 65536} [r eval_ro {
            return redis.call('BITGETINTARRAY', KEYS[1])
        } 1 intarray:script]
        assert_equal {5 11} [r eval_ro {
            return redis.call('BITRANGEINTARRAY', KEYS[1], ARGV[1], ARGV[2])
        } 1 intarray:script 1 2]
    }

    test {Integer-array commands preserve data, TTL, dirty count, and WATCH} {
        intarray_seed intarray:readonly {0 1 1000 65536}
        r pexpire intarray:readonly 600000
        set expire_at [r pexpiretime intarray:readonly]
        set dump [r dump intarray:readonly]
        set dirty [s rdb_changes_since_last_save]
        r watch intarray:readonly
        assert_equal {0 1 1000 65536} [r bitgetintarray intarray:readonly]
        assert_equal {1 1000} [r bitrangeintarray intarray:readonly 1 2]
        assert_equal $expire_at [r pexpiretime intarray:readonly]
        assert_equal $dump [r dump intarray:readonly]
        assert_equal $dirty [s rdb_changes_since_last_save]
        r multi
        r ping
        assert_equal {PONG} [r exec]
    }

    test {Integer-array commands require read key permission} {
        intarray_seed intarray:acl {3 17}
        r acl setuser intarray-reader reset on nopass -@all \
            +bitgetintarray +bitrangeintarray %R~intarray:acl
        set client [redis_client]
        $client auth intarray-reader password
        assert_equal {3 17} [$client bitgetintarray intarray:acl]
        assert_equal {17} [$client bitrangeintarray intarray:acl 1 1]
        assert_error {NOPERM*} {$client bitgetintarray intarray:other}
        r acl setuser intarray-reader resetkeys %W~intarray:acl
        assert_error {NOPERM*} {$client bitgetintarray intarray:acl}
        assert_error {NOPERM*} {$client bitrangeintarray intarray:acl 0 1}
        $client close
        r acl deluser intarray-reader
    } undefined {external:skip}

    test {Integer-array commands do not emit keyspace mutation notifications} {
        intarray_seed intarray:notifications {1 9}
        set old [config_get_set notify-keyspace-events KA]
        set client [redis_deferring_client]
        $client psubscribe __keyspace@*__:intarray:notifications
        $client read
        $client subscribe intarray:barrier
        $client read
        assert_equal {1 9} [r bitgetintarray intarray:notifications]
        assert_equal {1} [r bitrangeintarray intarray:notifications 0 0]
        # An ordinary published message is a barrier after both reads. Any
        # erroneous keyspace event would be received before this message.
        r publish intarray:barrier done
        assert_equal {message intarray:barrier done} [$client read]
        $client close
        r config set notify-keyspace-events $old
    } undefined {external:skip}

    test {Integer-array streaming stops when the client output limit closes the connection} {
        set old_bitmap [config_get_set bitmap-default-roaring yes]
        r del intarray:outputlimit
        r setbit intarray:outputlimit 1048575 0
        r bitop not intarray:outputlimit intarray:outputlimit
        r config set bitmap-default-roaring $old_bitmap
        assert_equal 1048576 [r bitcount intarray:outputlimit]
        set old_limit [config_get_set client-output-buffer-limit {normal 100000 0 0}]
        foreach args {{bitgetintarray intarray:outputlimit}
                      {bitrangeintarray intarray:outputlimit 0 1048575}} {
            set client [redis_deferring_client]
            $client client setname intarray-output-limit
            assert_equal OK [$client read]
            $client {*}$args
            $client flush
            wait_for_condition 100 10 {
                ![string match {*name=intarray-output-limit*} [r client list]]
            } else {
                fail "Integer-array reply did not enforce the client output limit"
            }
            assert_equal {} [$client rawread]
            $client close
        }
        r config set client-output-buffer-limit $old_limit
        r del intarray:outputlimit
    } undefined {external:skip}
}
