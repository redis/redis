# Build portable Roaring fixtures directly from the little-endian wire format,
# independently of Redis serialization and the vendored CRoaring library.
proc bitroar_command_le {value bytes} {
    set hex {}
    for {set i 0} {$i < $bytes} {incr i} {
        append hex [format %02x [expr {($value >> (8 * $i)) & 255}]]
    }
    return [binary format H* $hex]
}

proc bitroar_command_array32 {values} {
    # No-run cookie, container count, key/cardinality, offset, uint16 values.
    set payload [binary format H* 3a300000]
    if {[llength $values] == 0} {
        append payload [bitroar_command_le 0 4]
        return $payload
    }
    append payload [bitroar_command_le 1 4]
    append payload [bitroar_command_le 0 2]
    append payload [bitroar_command_le [expr {[llength $values] - 1}] 2]
    append payload [bitroar_command_le 16 4]
    foreach value $values {
        append payload [bitroar_command_le $value 2]
    }
    return $payload
}

proc bitroar_command_map64 {buckets} {
    set payload [bitroar_command_le [expr {[llength $buckets] / 2}] 8]
    foreach {high bitmap32} $buckets {
        append payload [bitroar_command_le $high 4] $bitmap32
    }
    return $payload
}

start_server {tags {"bitmap" "bitmap-roaring" "external:skip"}} {
    set empty32 [bitroar_command_array32 {}]
    set empty64 [bitroar_command_map64 {}]
    set array32 [bitroar_command_array32 {0 7 15}]
    set array64 [bitroar_command_map64 [list 0 $array32]]
    set bitset32 [binary format H* 3a300000010000000000ff7f10000000]
    append bitset32 [string repeat [binary format H* 55] 8192]

    test {BITROARSET imports an independent 32-bit portable array fixture} {
        assert_equal OK [r bitroarset portable:array32 4 $array32]
        assert_equal bitmap [r type portable:array32]
        assert_equal bitmap-roaring [r object encoding portable:array32]
        assert_equal 3 [r bitcount portable:array32]
        foreach offset {0 7 15} {
            assert_equal 1 [r getbit portable:array32 $offset]
        }
        foreach offset {1 6 8 14 16 31} {
            assert_equal 0 [r getbit portable:array32 $offset]
        }
        assert_equal [list 4 $array64] [r bitroarget portable:array32]
    }

    test {BITROARSET imports an independent 32-bit portable run fixture} {
        # One run container: 256 consecutive values from 0x1200 to 0x12ff.
        set run32 [binary format H* 3b300000010000ff0001000012ff00]
        assert_equal OK [r bitroarset portable:run32 608 $run32]
        assert_equal 256 [r bitcount portable:run32]
        assert_equal 0 [r getbit portable:run32 4607]
        assert_equal 1 [r getbit portable:run32 4608]
        assert_equal 1 [r getbit portable:run32 4863]
        assert_equal 0 [r getbit portable:run32 4864]
        assert_equal [list 608 [bitroar_command_map64 [list 0 $run32]]] \
            [r bitroarget portable:run32]
    }

    test {BITROARSET imports an independent 32-bit portable bitset fixture} {
        # A 32768-element bitset with every even bit set. Portable bits are
        # little-endian; Redis bitmap offsets still address the MSB first.
        assert_equal OK [r bitroarset portable:bitset32 8192 $bitset32]
        assert_equal 32768 [r bitcount portable:bitset32]
        foreach offset {0 2 64 32768 65534} {
            assert_equal 1 [r getbit portable:bitset32 $offset]
            assert_equal 0 [r getbit portable:bitset32 [expr {$offset + 1}]]
        }
        assert_equal [list 8192 [bitroar_command_map64 [list 0 $bitset32]]] \
            [r bitroarget portable:bitset32]
    }

    test {BITROARSET imports multiple 32-bit containers and 64-bit buckets} {
        # Two array containers: values 0 and 65541.
        set multi32 [binary format H* \
            3a300000020000000000000001000000180000001a00000000000500]
        assert_equal OK [r bitroarset portable:containers 8193 $multi32]
        assert_equal 2 [r bitcount portable:containers]
        assert_equal 1 [r getbit portable:containers 0]
        assert_equal 1 [r getbit portable:containers 65541]

        # Sparse high-32 buckets require no allocation proportional to length.
        set high64 [bitroar_command_map64 [list \
            0 [bitroar_command_array32 {3}] \
            1 [bitroar_command_array32 {5}]]]
        set length [expr {((1 << 32) + 5) / 8 + 1}]
        assert_equal OK [r bitroarset portable:buckets $length $high64]
        assert_equal 2 [r bitcount portable:buckets]
        assert_equal [list $length $high64] [r bitroarget portable:buckets]
        assert_lessthan [r memory usage portable:buckets] 65536
    }

    test {BITROARSET accepts the maximum unsigned 32-bit portable value} {
        # Container key and array value are both 0xffff, representing bit
        # 0xffffffff rather than a signed negative offset.
        set max32 [binary format H* 3a30000001000000ffff000010000000ffff]
        set length [expr {1 << 29}]
        assert_equal OK [r bitroarset portable:max32 $length $max32]
        assert_equal 1 [r bitcount portable:max32]
        assert_equal [list $length [bitroar_command_map64 [list 0 $max32]]] \
            [r bitroarget portable:max32]
        assert_error {ERR*} {r bitroarset portable:max32-short [expr {$length - 1}] $max32}
        assert_equal 0 [r exists portable:max32-short]
    }

    if {[s arch_bits] == 64} {
        test {64-bit portable bucket counts may match the 32-bit format cookies} {
            set singleton [bitroar_command_array32 {0}]
            foreach count {12346 12347} {
                # 12346 == 0x303a and 12347 == 0x303b: the first four bytes
                # look like a 32-bit cookie, but this is a complete 64-bit map.
                set payload [bitroar_command_le $count 8]
                for {set high 0} {$high < $count} {incr high} {
                    append payload [bitroar_command_le $high 4] $singleton
                }
                set length [expr {(($count - 1) << 29) + 1}]
                assert_equal OK [r bitroarset portable:cookie-collision $length $payload]
                assert_equal $count [r bitcount portable:cookie-collision]
                assert_equal [list $length $payload] [r bitroarget portable:cookie-collision]
            }
            r del portable:cookie-collision
        }
    }

    test {BITROARSET accepts empty 32-bit and 64-bit payloads and preserves length} {
        foreach payload [list $empty32 $empty64] {
            foreach length {0 1 123456789} {
                assert_equal OK [r bitroarset portable:empty $length $payload]
                assert_equal bitmap [r type portable:empty]
                assert_equal 0 [r bitcount portable:empty]
                assert_equal [list $length $empty64] [r bitroarget portable:empty]
            }
        }
    }

    test {BITROARGET output round-trips without losing trailing zero bytes} {
        assert_equal OK [r bitroarset portable:original 100 $array32]
        lassign [r bitroarget portable:original] length payload
        assert_equal OK [r bitroarset portable:copy $length $payload]
        assert_equal [r bitroarget portable:original] [r bitroarget portable:copy]
        assert_equal [r dump portable:original] [r dump portable:copy]
    }

    test {BITROARGET returns null for missing keys and rejects other types} {
        r del portable:missing
        assert_equal {} [r bitroarget portable:missing]
        r set portable:string value
        r lpush portable:list value
        foreach key {portable:string portable:list} {
            assert_error {WRONGTYPE*} {r bitroarget $key}
        }
    }

    test {Portable bitmap commands do not depend on bitmap-default-roaring} {
        set old_default [lindex [r config get bitmap-default-roaring] 1]
        foreach mode {no yes} {
            r config set bitmap-default-roaring $mode
            assert_equal OK [r bitroarset portable:config 2 $array64]
            assert_equal bitmap [r type portable:config]
            assert_equal [list 2 $array64] [r bitroarget portable:config]
        }
        r config set bitmap-default-roaring $old_default
    }

    test {Portable bitmap logical length is independent of proto-max-bulk-len} {
        set old_limit [lindex [r config get proto-max-bulk-len] 1]
        r config set proto-max-bulk-len 1048576
        set max_length [expr {[s arch_bits] == 64 ? (1 << 60) - 1 : (1 << 32) - 1}]
        foreach length [list 1048577 $max_length] {
            assert_equal OK [r bitroarset portable:large $length $array64]
            assert_equal [list $length $array64] [r bitroarget portable:large]
            assert_equal 3 [r bitcount portable:large]
            assert_lessthan [r memory usage portable:large] 65536
        }
        r config set proto-max-bulk-len $old_limit
    }

    test {BITROARSET rejects invalid lengths without changing the destination} {
        r set portable:invalid original
        r pexpire portable:invalid 600000
        set expire [r pexpiretime portable:invalid]
        set max_length [expr {[s arch_bits] == 64 ? (1 << 60) - 1 : (1 << 32) - 1}]
        foreach length [list -1 abc 1.5 9223372036854775808 [expr {$max_length + 1}]] {
            assert_error {ERR*} {r bitroarset portable:invalid $length $empty64}
            assert_equal original [r get portable:invalid]
            assert_equal $expire [r pexpiretime portable:invalid]
        }
    }

    test {BITROARSET rejects bits outside the logical length before replacing a key} {
        r bitroarset portable:bounded 2 $array32
        set before [r bitroarget portable:bounded]
        foreach {length values} {0 {0} 1 {8} 2 {16}} {
            foreach payload [list [bitroar_command_array32 $values] \
                [bitroar_command_map64 [list 0 [bitroar_command_array32 $values]]]] {
                assert_error {ERR*} {r bitroarset portable:bounded $length $payload}
                assert_equal $before [r bitroarget portable:bounded]
            }
        }
        set outside64 [bitroar_command_map64 [list 1 [bitroar_command_array32 {0}]]]
        assert_error {ERR*} {
            r bitroarset portable:bounded [expr {(1 << 32) / 8}] $outside64
        }
        assert_equal $before [r bitroarget portable:bounded]
    }

    test {BITROARSET rejects truncated and trailing portable data atomically} {
        r bitroarset portable:invalid-data 2 $array32
        r pexpire portable:invalid-data 600000
        set before [r bitroarget portable:invalid-data]
        set expire [r pexpiretime portable:invalid-data]
        foreach complete [list $array32 $array64] {
            for {set cut 0} {$cut < [string length $complete]} {incr cut} {
                set payload [string range $complete 0 [expr {$cut - 1}]]
                assert_error {ERR*} {r bitroarset portable:invalid-data 2 $payload}
                assert_equal $before [r bitroarget portable:invalid-data]
                assert_equal $expire [r pexpiretime portable:invalid-data]
            }
            assert_error {ERR*} {
                r bitroarset portable:invalid-data 2 ${complete}[binary format H* 00]
            }
        }
        assert_equal $before [r bitroarget portable:invalid-data]
        assert_equal $expire [r pexpiretime portable:invalid-data]
    }

    test {BITROARSET rejects malformed container contents and bucket ordering} {
        set malformed [list [binary format H* deadbeef] \
            [bitroar_command_array32 {7 0}] [bitroar_command_array32 {7 7}]]
        # Declared cardinality disagrees with the bitset's actual population.
        lappend malformed [string replace $bitset32 end end [binary format H* 00]]
        # Run intervals overlap, despite matching the declared cardinality.
        lappend malformed [binary format H* 3b300000010000010002000000000000000000]
        # High-32 keys must be unique and increasing.
        foreach buckets [list \
            [list 0 $array32 0 $array32] \
            [list 1 $array32 0 $array32]] {
            lappend malformed [bitroar_command_map64 $buckets]
        }
        r del portable:malformed
        foreach payload $malformed {
            assert_error {ERR*} {r bitroarset portable:malformed 536870914 $payload}
            assert_equal 0 [r exists portable:malformed]
        }
    }

    test {BITROARSET replaces values of any type and clears expiration} {
        foreach kind {string list bitmap} {
            r del portable:replace
            switch $kind {
                string {r set portable:replace value}
                list {r lpush portable:replace value}
                bitmap {r bitroarset portable:replace 100 $empty64}
            }
            r pexpire portable:replace 600000
            assert_equal OK [r bitroarset portable:replace 2 $array32]
            assert_equal [list 2 $array64] [r bitroarget portable:replace]
            assert_equal -1 [r pttl portable:replace]
        }
    }

    test {BITROARSET MERGE unions bits and preserves maximum length and expiration} {
        r bitroarset portable:merge 20 [bitroar_command_array32 {0 8}]
        r pexpire portable:merge 600000
        set expire [r pexpiretime portable:merge]
        assert_equal OK [r bitroarset portable:merge 2 $array32 MERGE]
        set merged [bitroar_command_map64 [list 0 [bitroar_command_array32 {0 7 8 15}]]]
        assert_equal [list 20 $merged] [r bitroarget portable:merge]
        assert_equal $expire [r pexpiretime portable:merge]
        assert_equal OK [r bitroarset portable:merge 30 $empty64 merge]
        assert_equal [list 30 $merged] [r bitroarget portable:merge]
        assert_equal $expire [r pexpiretime portable:merge]

        r del portable:merge-missing
        assert_equal OK [r bitroarset portable:merge-missing 2 $array32 MERGE]
        assert_equal [list 2 $array64] [r bitroarget portable:merge-missing]
    }

    test {BITROARSET MERGE rejects other types and malformed input without mutation} {
        r set portable:merge-string value
        r pexpire portable:merge-string 600000
        set expire [r pexpiretime portable:merge-string]
        assert_error {WRONGTYPE*} {r bitroarset portable:merge-string 2 $array32 MERGE}
        assert_equal value [r get portable:merge-string]
        assert_equal $expire [r pexpiretime portable:merge-string]

        r bitroarset portable:merge-invalid 20 $array32
        set before [r bitroarget portable:merge-invalid]
        assert_error {ERR*} {r bitroarset portable:merge-invalid 1 $array32 MERGE}
        assert_error {ERR*} {r bitroarset portable:merge-invalid 2 invalid MERGE}
        assert_equal $before [r bitroarget portable:merge-invalid]
    }

    test {Portable bitmap commands reject invalid syntax and report their key} {
        assert_error {*wrong number of arguments*} {r bitroarset portable:syntax 2}
        assert_error {*wrong number of arguments*} {r bitroarget}
        assert_error {*wrong number of arguments*} {r bitroarget portable:syntax extra}
        foreach options {{UNKNOWN} {MERGE MERGE} {MERGE extra}} {
            assert_error {ERR*} {r bitroarset portable:syntax 2 $array32 {*}$options}
        }
        assert_equal 0 [r exists portable:syntax]
        assert_equal {portable:syntax} [r command getkeys bitroarset portable:syntax 2 $array32]
        assert_equal {portable:syntax} [r command getkeys bitroarset portable:syntax 2 $array32 MERGE]
        assert_equal {portable:syntax} [r command getkeys bitroarget portable:syntax]
        assert_equal {{portable:syntax {OW update}}} \
            [r command getkeysandflags bitroarset portable:syntax 2 $array32]
        assert_equal {{portable:syntax {RW access update}}} \
            [r command getkeysandflags bitroarset portable:syntax 2 $array32 MERGE]
        assert_equal {{portable:syntax {RO access}}} \
            [r command getkeysandflags bitroarget portable:syntax]
    }

    test {Portable bitmap ACLs distinguish replacement from read-modify-write} {
        set client [redis_client]
        r acl setuser portable-user reset on nopass +@bitmap %W~portable:acl
        $client auth portable-user ignored
        assert_equal OK [$client bitroarset portable:acl 2 $array32]
        assert_error {NOPERM*key*} {$client bitroarget portable:acl}
        assert_error {NOPERM*key*} {$client bitroarset portable:acl 2 $array32 MERGE}

        r acl setuser portable-user %R~portable:acl
        assert_equal [list 2 $array64] [$client bitroarget portable:acl]
        assert_equal OK [$client bitroarset portable:acl 2 $array32 MERGE]
        assert_error {NOPERM*key*} {$client bitroarset portable:acl-other 2 $array32}

        r acl setuser portable-user -@bitmap
        assert_error {NOPERM*command*} {$client bitroarget portable:acl}
        assert_error {NOPERM*command*} {$client bitroarset portable:acl 2 $array32}
        $client close
        r acl deluser portable-user
    }

    test {BITROARSET publishes bitmap events and MERGE does not overwrite the key} {
        set old_notifications [lindex [r config get notify-keyspace-events] 1]
        r config set notify-keyspace-events {}
        r bitroarset portable:notify 2 $array32
        set db [expr {$::singledb ? 0 : 9}]
        set channel __keyevent@${db}__
        set rd [redis_deferring_client]
        $rd psubscribe ${channel}:*
        $rd read
        r config set notify-keyspace-events Ebo

        r bitroarset portable:notify 2 $array32
        assert_equal [list pmessage ${channel}:* ${channel}:overwritten portable:notify] [$rd read]
        assert_equal [list pmessage ${channel}:* ${channel}:bitroarset portable:notify] [$rd read]

        r bitroarset portable:notify 2 $array32 MERGE
        r publish ${channel}:barrier done
        assert_equal [list pmessage ${channel}:* ${channel}:bitroarset portable:notify] [$rd read]
        assert_equal [list pmessage ${channel}:* ${channel}:barrier done] [$rd read]

        # Reads and rejected imports must not publish write notifications.
        r bitroarget portable:notify
        assert_error {ERR*} {r bitroarset portable:notify 2 invalid}
        r publish ${channel}:barrier done
        assert_equal [list pmessage ${channel}:* ${channel}:barrier done] [$rd read]

        # The event belongs to the bitmap class, not the string class.
        r config set notify-keyspace-events E\$
        r bitroarset portable:notify 2 $array32
        r publish ${channel}:barrier done
        assert_equal [list pmessage ${channel}:* ${channel}:barrier done] [$rd read]
        $rd close
        r config set notify-keyspace-events $old_notifications
    }

    test {Portable bitmap commands work in scripts and transactions} {
        assert_equal [list 2 $array64] [r eval {
            redis.call('BITROARSET', KEYS[1], ARGV[1], ARGV[2])
            return redis.call('BITROARGET', KEYS[1])
        } 1 portable:script 2 $array32]
        r multi
        r bitroarset portable:transaction 2 $array32
        r bitroarget portable:transaction
        assert_equal [list OK [list 2 $array64]] [r exec]
    }

    test {BITROARSET invalidates WATCH while reads and rejected imports do not} {
        r bitroarset portable:watch 2 $array32
        r watch portable:watch
        r bitroarget portable:watch
        assert_error {ERR*} {r bitroarset portable:watch 2 invalid}
        r multi
        r ping
        assert_equal {PONG} [r exec]

        foreach options {{} {MERGE}} {
            r watch portable:watch
            r bitroarset portable:watch 2 [bitroar_command_array32 {1}] {*}$options
            r multi
            r ping
            assert_equal {} [r exec]
        }
    }
}

start_server {tags {"bitmap" "bitmap-roaring" "repl" "aof" "needs:debug" "external:skip" "cluster:skip" "logreqres:skip"} overrides {appendonly yes appendfsync always save {} aof-use-rdb-preamble no auto-aof-rewrite-percentage 0}} {
    start_server {overrides {appendonly yes appendfsync always save {} aof-use-rdb-preamble no auto-aof-rewrite-percentage 0}} {
        set master [srv -1 client]
        set replica [srv 0 client]
        $replica replicaof [srv -1 host] [srv -1 port]
        wait_for_sync $replica
        $master config set proto-max-bulk-len 1048576
        $replica config set proto-max-bulk-len 1048576
        set replay_expected {}

        foreach {master_default replica_default} {no yes yes no} {
            test "Portable bitmap imports replicate with opposite defaults (primary $master_default)" {
                $master config set bitmap-default-roaring $master_default
                $replica config set bitmap-default-roaring $replica_default
                set prefix portable:repl:$master_default

                $master set $prefix:replace old PX 600000
                assert_equal OK [$master bitroarset $prefix:replace 4 \
                    [bitroar_command_array32 {0 7}]]
                assert_equal -1 [$master pexpiretime $prefix:replace]

                $master bitroarset $prefix:merge 2 [bitroar_command_array32 {0}]
                $master pexpire $prefix:merge 600000
                set expire_at [$master pexpiretime $prefix:merge]
                assert_equal OK [$master bitroarset $prefix:merge 8 \
                    [bitroar_command_array32 {15}] MERGE]
                assert_equal 2 [$master bitcount $prefix:merge]
                assert_equal $expire_at [$master pexpiretime $prefix:merge]

                # The compact payload crosses the 32-bit offset boundary while
                # its logical length exceeds both nodes' protocol limits.
                set sparse [bitroar_command_map64 [list 1 [bitroar_command_array32 {5}]]]
                set length [expr {((1 << 32) + 5) / 8 + 1}]
                assert_equal OK [$master bitroarset $prefix:sparse $length $sparse]
                assert_equal [list $length $sparse] [$master bitroarget $prefix:sparse]
                assert_equal 1 [$master bitcount $prefix:sparse]

                wait_for_ofs_sync $master $replica
                foreach suffix {replace merge sparse} {
                    set key $prefix:$suffix
                    assert_equal bitmap [$replica type $key]
                    assert_equal [$master bitroarget $key] [$replica bitroarget $key]
                    assert_equal [$master pexpiretime $key] [$replica pexpiretime $key]
                    lappend replay_expected $key [$master bitroarget $key] [$master pexpiretime $key]
                }
            }
        }

        test {Portable bitmap imports expire old destinations before propagation} {
            $master debug set-active-expire 0
            foreach mode {replace merge} {
                set key portable:repl:expired:$mode
                $master set $key old PX 1
            }
            after 10

            foreach mode {replace merge} {
                set key portable:repl:expired:$mode
                set options [expr {$mode eq "merge" ? [list MERGE] : {}}]
                assert_equal OK [$master bitroarset $key 1 \
                    [bitroar_command_array32 {7}] {*}$options]
                lappend replay_expected $key [$master bitroarget $key] -1
            }
            $master debug set-active-expire 1
            wait_for_ofs_sync $master $replica
            foreach mode {replace merge} {
                set key portable:repl:expired:$mode
                assert_equal [$master bitroarget $key] [$replica bitroarget $key]
                assert_equal 1 [$replica bitcount $key]
                assert_equal -1 [$replica pexpiretime $key]
            }
        }

        test {Portable bitmap imports replay from primary and replica AOF with either default} {
            $replica replicaof no one
            foreach default {no yes} {
                foreach client [list $master $replica] {
                    $client config set bitmap-default-roaring $default
                    $client debug loadaof
                    foreach {key expected expire_at} $replay_expected {
                        assert_equal bitmap [$client type $key]
                        assert_equal $expected [$client bitroarget $key]
                        assert_equal $expire_at [$client pexpiretime $key]
                    }
                }
            }
        }
    }
}

start_server {tags {"bitmap" "bitmap-roaring" "modules" "repl" "aof" "needs:debug" "external:skip" "cluster:skip" "logreqres:skip"}} {
    set replica [srv 0 client]
    set notification_module [file normalize tests/modules/keyspace_events.so]
    start_server [list overrides [list loadmodule $notification_module appendonly yes \
        appendfsync always save {} aof-use-rdb-preamble no auto-aof-rewrite-percentage 0]] {
        set master [srv 0 client]
        $replica replicaof [srv 0 host] [srv 0 port]
        wait_for_sync $replica

        test {Portable bitmap imports propagate before synchronous bitmap callbacks} {
            set payload [bitroar_command_array32 {0}]
            $master bitroarset portable:callback:seed 1 $payload
            set seed [$master dump portable:callback:seed]
            foreach mode {replace merge} {
                set key bitmap:transition:delete-bitmap:portable:$mode
                set options {}
                if {$mode eq "merge"} {
                    # RESTORE establishes the native target without triggering
                    # the bitmap callback, which deletes this key prefix.
                    $master restore $key 0 $seed
                    lappend options MERGE
                }
                assert_equal OK [$master bitroarset $key 1 $payload {*}$options]
                assert_equal 0 [$master exists $key]
                wait_for_ofs_sync $master $replica
                assert_equal 0 [$replica exists $key]
            }

            # The callback runs only on a live primary. Replay must therefore
            # receive the callback's DEL after the import that triggered it.
            $replica replicaof no one
            $master debug loadaof
            foreach mode {replace merge} {
                assert_equal 0 [$master exists bitmap:transition:delete-bitmap:portable:$mode]
            }
        }
    }
}
