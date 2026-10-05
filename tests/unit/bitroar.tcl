set sparse_public_offset 65536
set sparse_public_len 8193

proc create_roaring_bitmap_from_raw {client key raw} {
    $client set $key $raw
    convert_string_bitmap_to_roaring $client $key
}

proc create_roaring_bitmap_from_bits {client key bits} {
    set old [lindex [$client config get bitmap-default-roaring] 1]

    $client del $key
    if {[llength $bits] == 0} {
        $client restore $key 0 [empty_roaring_bitmap_dump_payload] replace
        return OK
    }

    $client config set bitmap-default-roaring yes
    set code [catch {
        foreach bit $bits {
            $client setbit $key $bit 1
        }
    } result opts]
    $client config set bitmap-default-roaring $old
    if {$code != 0} {
        return -options $opts $result
    }
    return OK
}

proc seed_roaring_bitmap {key bits} {
    create_roaring_bitmap_from_bits r $key $bits
}

# Extract the raw string payload from a bitmap DUMP. It starts with the RDB
# type byte followed by the logical byte length and the portable blob length.
# Tests using this helper disable RDB compression, so both lengths use ordinary
# RDB length encodings and the payload ends before the two-byte RDB version and
# eight-byte checksum.
proc roaring_portable_payload {dump} {
    set offset 1
    foreach field {byte-length payload-length} {
        binary scan [string index $dump $offset] cu first
        set type [expr {$first >> 6}]
        if {$type == 0} {
            incr offset
        } elseif {$type == 1} {
            incr offset 2
        } elseif {$first == 0x80} {
            incr offset 5
        } elseif {$first == 0x81} {
            incr offset 9
        } else {
            fail "unexpected encoded $field in bitmap DUMP"
        }
    }
    return [string range $dump $offset end-10]
}

start_server {tags {"bitmap" "bitmap-roaring" "needs:debug" "cluster:skip"}} {
    # Configuration and type-transition coverage establishes when bitmap
    # commands create a native value and which string semantics remain intact.
    test {bitmap-default-roaring defaults to no} {
        assert_equal no [lindex [r config get bitmap-default-roaring] 1]
    }

    test {BITOP preserves a destination key named ROARING and fixed key specs} {
        r config set bitmap-default-roaring no
        r del ROARING bitmap_source
        r set bitmap_source [binary format H* f0]

        assert_equal {ROARING bitmap_source} \
            [r command getkeys bitop and ROARING bitmap_source]
        assert_equal {{ROARING {OW update}} {bitmap_source {RO access}}} \
            [r command getkeysandflags bitop and ROARING bitmap_source]

        # ROARING remains an ordinary destination key under the historical
        # grammar, regardless of the configured destination representation.
        assert_equal 1 [r bitop and ROARING bitmap_source]
        assert_equal string [r type ROARING]
        assert_equal [binary format H* f0] [r get ROARING]

        r config set bitmap-default-roaring yes
        assert_equal 1 [r bitop and ROARING bitmap_source]
        assert_equal bitmap [r type ROARING]
        assert_equal [binary format H* f0] [r debug bitmap-raw ROARING]
        r config set bitmap-default-roaring no
    }

    test {Internal BITCONVERT replay primitive has narrow semantics} {
        set raw [binary format H* 80400100080000]
        r del bitmap_missing bitmap_string bitmap_list

        # Ordinary clients cannot discover the primitive through COMMAND or execute it.
        assert_equal {{}} [r command info bitconvert]
        assert_error {ERR unknown command 'bitconvert'*} {
            r bitconvert bitmap_missing
        }

        r debug mark-internal-client

        # Key specs stay introspectable for the internal primitive.
        assert_equal {bitmap_missing} \
            [r command getkeys bitconvert bitmap_missing]
        assert_equal {{bitmap_missing {RW access update}}} \
            [r command getkeysandflags bitconvert bitmap_missing]

        # A missing key becomes an empty native bitmap.
        assert_equal OK [r bitconvert bitmap_missing]
        assert_equal bitmap [r type bitmap_missing]
        assert_equal bitmap-roaring [r object encoding bitmap_missing]
        assert_equal {} [r debug bitmap-raw bitmap_missing]

        # Strings convert in place without changing bytes or expiration.
        r set bitmap_string $raw
        r pexpire bitmap_string 600000
        set expire_at [r pexpiretime bitmap_string]
        assert_equal OK [r bitconvert bitmap_string]
        assert_equal bitmap [r type bitmap_string]
        assert_equal $raw [r debug bitmap-raw bitmap_string]
        assert_equal $expire_at [r pexpiretime bitmap_string]

        # Converting a native bitmap again is an idempotent no-op.
        set digest [r debug digest-value bitmap_string]
        assert_equal OK [r bitconvert bitmap_string]
        assert_equal $digest [r debug digest-value bitmap_string]
        assert_equal $expire_at [r pexpiretime bitmap_string]

        # Other value types retain their ordinary WRONGTYPE behavior.
        r lpush bitmap_list value
        assert_error {WRONGTYPE*} {r bitconvert bitmap_list}
        assert_equal list [r type bitmap_list]

        # The conversion target is implied; extra arguments fail arity.
        assert_error {ERR wrong number of arguments*} {
            r bitconvert bitmap_missing ROARING
        }

        r debug mark-internal-client unmark
        assert_error {ERR unknown command 'bitconvert'*} {
            r bitconvert bitmap_missing
        }
    }

    test {Internal BITROAROP replay stores native results independently of config} {
        assert_equal {{}} [r command info bitroarop]
        assert_error {ERR unknown command 'bitroarop'*} {
            r bitroarop or bitmap_bitop_out bitmap_bitop_source
        }

        r debug mark-internal-client
        assert_equal {bitmap_bitop_out bitmap_bitop_source} \
            [r command getkeys bitroarop or bitmap_bitop_out bitmap_bitop_source]
        assert_equal {{bitmap_bitop_out {OW update}} {bitmap_bitop_source {RO access}}} \
            [r command getkeysandflags bitroarop or bitmap_bitop_out bitmap_bitop_source]

        r config set bitmap-default-roaring no
        r set bitmap_bitop_source [binary format H* f0]
        assert_equal 1 [r bitroarop or bitmap_bitop_out bitmap_bitop_source]
        assert_equal bitmap [r type bitmap_bitop_out]
        assert_equal [binary format H* f0] [r debug bitmap-raw bitmap_bitop_out]
        # Destination/source aliasing must read the original string first.
        assert_equal 1 [r bitroarop not bitmap_bitop_source bitmap_bitop_source]
        assert_equal [binary format H* 0f] [r debug bitmap-raw bitmap_bitop_source]

        r debug mark-internal-client unmark
        assert_error {ERR unknown command 'bitroarop'*} {
            r bitroarop or bitmap_bitop_out bitmap_bitop_source
        }
    }

    test {Internal bitmap propagation primitives are unreachable from scripts and MULTI} {
        r config set bitmap-default-roaring no
        r del bitmap_gate bitmap_gate_out
        r set bitmap_gate [binary format H* f0]
        r function load replace {#!lua name=bitmap_gate
            redis.register_function('call_bitconvert', function(KEYS, ARGV)
                return redis.call('bitconvert', KEYS[1])
            end)
            redis.register_function('call_bitroarop', function(KEYS, ARGV)
                return redis.call('bitroarop', 'or', KEYS[1], KEYS[2])
            end)
        }

        # Scripts only honor NOSCRIPT, not the internal-client check, so both
        # primitives must stay NOSCRIPT to be rejected, even for internal clients.
        foreach internal {0 1} {
            if {$internal} {r debug mark-internal-client}
            assert_error {*not allowed from script*} {
                r eval {return redis.call('bitconvert', KEYS[1])} 1 bitmap_gate
            }
            assert_error {*not allowed from script*} {
                r eval {return redis.call('bitroarop', 'or', KEYS[1], KEYS[2])} \
                    2 bitmap_gate_out bitmap_gate
            }
            assert_error {*not allowed from script*} {
                r fcall call_bitconvert 1 bitmap_gate
            }
            assert_error {*not allowed from script*} {
                r fcall call_bitroarop 2 bitmap_gate_out bitmap_gate
            }
        }
        r debug mark-internal-client unmark

        # Ordinary clients cannot queue them either, which aborts the transaction.
        r multi
        assert_error {ERR unknown command 'bitconvert'*} {r bitconvert bitmap_gate}
        assert_error {ERR unknown command 'bitroarop'*} {
            r bitroarop or bitmap_gate_out bitmap_gate
        }
        assert_error {EXECABORT*} {r exec}

        # No path changed the representation or created the destination.
        assert_equal string [r type bitmap_gate]
        assert_equal [binary format H* f0] [r get bitmap_gate]
        assert_equal 0 [r exists bitmap_gate_out]
        r function delete bitmap_gate
    }

    test {Internal bitmap propagation primitives cannot be renamed} {
        foreach command {bitconvert bitroarop} {
            catch {exec src/redis-server --rename-command $command renamed} err
            assert_match {*Cannot rename an internal command*} $err
        }
    } {} {external:skip}

    test {bitmap-default-roaring no: SETBIT keeps creating strings} {
        r config set bitmap-default-roaring no
        r del bitmap bitmap:existing

        assert_equal 0 [r setbit bitmap $sparse_public_offset 1]
        assert_equal string [r type bitmap]
        assert_equal $sparse_public_len [r strlen bitmap]
        assert_equal 1 [r getbit bitmap $sparse_public_offset]

        r set bitmap:existing [binary format H* 80]
        assert_equal 0 [r setbit bitmap:existing 1 1]
        assert_equal string [r type bitmap:existing]
        assert_equal [binary format H* c0] [r get bitmap:existing]
    }

    test {bitmap-default-roaring yes: SETBIT creates Roaring bitmaps for missing keys} {
        r config set bitmap-default-roaring yes
        r del bitmap

        assert_equal 0 [r setbit bitmap $sparse_public_offset 1]
        assert_equal bitmap [r type bitmap]
        assert_equal bitmap-roaring [r object encoding bitmap]
        assert_match {*encoding:bitmap-roaring*} [r debug object bitmap]
        assert_equal 1 [r getbit bitmap $sparse_public_offset]
        assert_equal 1 [r bitcount bitmap]
        assert_equal $sparse_public_len [string length [r debug bitmap-raw bitmap]]
        assert_error {WRONGTYPE*} {r get bitmap}
        r config set bitmap-default-roaring no
    }

    test {bitmap-default-roaring yes: SETBIT converts existing string values and keeps TTL} {
        r config set bitmap-default-roaring yes

        r set bitmap ""
        r pexpire bitmap 60000
        set expire_at [r pexpiretime bitmap]
        assert_equal 0 [r setbit bitmap $sparse_public_offset 1]
        assert_equal bitmap [r type bitmap]
        assert_equal bitmap-roaring [r object encoding bitmap]
        assert_equal 1 [r getbit bitmap $sparse_public_offset]
        assert_equal $expire_at [r pexpiretime bitmap]
        assert_error {WRONGTYPE*} {r get bitmap}
        r config set bitmap-default-roaring no
    }

    test {bitmap-default-roaring yes: conversion preserves existing string content} {
        r del bitmap:public:content
        # Plain SET always writes a string, in either mode; only bitmap
        # command writes convert.
        r config set bitmap-default-roaring yes
        r set bitmap:public:content [binary format H* f00f]
        assert_equal string [r type bitmap:public:content]

        # The converting SETBIT reports the old bit value read from the
        # original string content.
        assert_equal 1 [r setbit bitmap:public:content 0 0]
        assert_equal bitmap [r type bitmap:public:content]
        assert_equal [binary format H* 700f] [r debug bitmap-raw bitmap:public:content]
        r config set bitmap-default-roaring no
    }

    test {bitmap-default-roaring yes: zero SETBIT extends Roaring bitmap length} {
        r config set bitmap-default-roaring yes
        r del bitmap:public:zero:new bitmap:public:zero:convert \
            bitmap:public:zero:existing

        set dirty [s rdb_changes_since_last_save]
        assert_equal 0 [r setbit bitmap:public:zero:new 0 0]
        assert_equal bitmap [r type bitmap:public:zero:new]
        assert_equal [binary format H* 00] [r debug bitmap-raw bitmap:public:zero:new]
        assert_equal [expr {$dirty + 1}] [s rdb_changes_since_last_save]

        r set bitmap:public:zero:convert ""
        set dirty [s rdb_changes_since_last_save]
        assert_equal 0 [r setbit bitmap:public:zero:convert 0 0]
        assert_equal bitmap [r type bitmap:public:zero:convert]
        assert_equal [binary format H* 00] [r debug bitmap-raw bitmap:public:zero:convert]
        assert_equal [expr {$dirty + 1}] [s rdb_changes_since_last_save]

        r config set bitmap-default-roaring no
    }

    test {bitmap-default-roaring yes: BITFIELD creates and converts Roaring bitmaps} {
        r config set bitmap-default-roaring yes
        r del bitmap:public:bf:new bitmap:public:bf:conv

        assert_equal {0} [r bitfield bitmap:public:bf:new SET u8 0 255]
        assert_equal bitmap [r type bitmap:public:bf:new]
        assert_equal 8 [r bitcount bitmap:public:bf:new]

        r set bitmap:public:bf:conv [binary format H* 01]
        assert_equal string [r type bitmap:public:bf:conv]
        assert_equal {2} [r bitfield bitmap:public:bf:conv INCRBY u8 0 1]
        assert_equal bitmap [r type bitmap:public:bf:conv]
        assert_equal [binary format H* 02] [r debug bitmap-raw bitmap:public:bf:conv]
        r config set bitmap-default-roaring no
    }

    test {bitmap-default-roaring yes: converting SETBIT and BITFIELD count one LFU access} {
        # With lfu-log-factor 0 every access increments the LFU counter, and
        # with lfu-decay-time 0 it never decays, so OBJECT FREQ counts the
        # accesses exactly. A conversion must count the command's access once,
        # like the string path does, and keep a string's earlier accesses.
        r config set maxmemory-policy allkeys-lfu
        r config set lfu-log-factor 0
        r config set lfu-decay-time 0

        set keys {lfu:setbit:new lfu:bitfield:new lfu:setbit:str lfu:bitfield:str}
        foreach roaring {no yes} {
            r config set bitmap-default-roaring $roaring
            r del {*}$keys

            r setbit lfu:setbit:new 7 1
            r bitfield lfu:bitfield:new SET u8 0 255
            foreach key {lfu:setbit:str lfu:bitfield:str} {
                # SET creates the string at LFU_INIT_VAL (5); two reads
                # bring it to 7.
                r set $key [binary format H* 00]
                r get $key
                r get $key
            }
            r setbit lfu:setbit:str 7 1
            r bitfield lfu:bitfield:str SET u8 0 255

            set type [expr {$roaring eq "yes" ? "bitmap" : "string"}]
            foreach key $keys {
                assert_equal [r type $key] $type
            }
            # New keys keep LFU_INIT_VAL; the strings get one more access.
            set freqs {}
            foreach key $keys {
                lappend freqs [r object freq $key]
            }
            assert_equal $freqs {5 5 8 8} "bitmap-default-roaring $roaring"
        }

        r del {*}$keys
        set _ {}
    } {} {needs:config-maxmemory config:restore}

    test {bitmap-default-roaring converts non-empty strings to Roaring bitmaps and keeps TTL} {
        r config set bitmap-default-roaring yes
        set raw [binary format H* 80400100080000]

        r del bitmap:convert
        r set bitmap:convert $raw
        r pexpire bitmap:convert 60000
        assert_equal 1 [r setbit bitmap:convert 0 1]
        assert_equal bitmap [r type bitmap:convert]
        assert_equal bitmap-roaring [r object encoding bitmap:convert]
        assert_equal $raw [r debug bitmap-raw bitmap:convert]
        assert {[r pttl bitmap:convert] > 0}
        r config set bitmap-default-roaring no
    }

    test {Roaring bitmap dump restore preserves all-zero logical byte length} {
        r config set bitmap-default-roaring no
        set raw [string repeat [binary format H* 00] 6]

        r del bitmap:convert:zeros bitmap:convert:zeros:restored
        r set bitmap:convert:zeros $raw
        assert_equal OK [convert_string_bitmap_to_roaring r bitmap:convert:zeros]
        assert_equal bitmap [r type bitmap:convert:zeros]
        assert_equal bitmap-roaring [r object encoding bitmap:convert:zeros]
        assert_equal 0 [r bitcount bitmap:convert:zeros]
        assert_equal $raw [r debug bitmap-raw bitmap:convert:zeros]

        set payload [r dump bitmap:convert:zeros]
        r restore bitmap:convert:zeros:restored 0 $payload
        assert_equal bitmap [r type bitmap:convert:zeros:restored]
        assert_equal bitmap-roaring [r object encoding bitmap:convert:zeros:restored]
        assert_equal 0 [r bitcount bitmap:convert:zeros:restored]
        assert_equal $raw [r debug bitmap-raw bitmap:convert:zeros:restored]
    }

    test {empty Roaring bitmap fixtures preserve zero logical byte length} {
        r del bitmap:fixture:empty
        assert_equal OK [create_roaring_bitmap_from_raw r bitmap:fixture:empty ""]
        assert_equal bitmap [r type bitmap:fixture:empty]
        assert_equal bitmap-roaring [r object encoding bitmap:fixture:empty]
        assert_equal "" [r debug bitmap-raw bitmap:fixture:empty]
        assert_equal -1 [r bitpos bitmap:fixture:empty 0]
        assert_equal -1 [r bitpos bitmap:fixture:empty 1]
    }

    test {bitmap-default-roaring conversion handles int-encoded strings and wrong types} {
        r del bitmap:convert:int bitmap:convert:list
        r set bitmap:convert:int 12345
        assert_equal int [r object encoding bitmap:convert:int]
        r config set bitmap-default-roaring yes
        assert_equal 0 [r setbit bitmap:convert:int 0 0]
        assert_equal bitmap [r type bitmap:convert:int]
        assert_equal "12345" [r debug bitmap-raw bitmap:convert:int]

        r rpush bitmap:convert:list element
        assert_error {WRONGTYPE*} {r setbit bitmap:convert:list 0 1}
        r config set bitmap-default-roaring no
    }

    # Digest and bounds tests protect logical length independently from the
    # history-dependent CRoaring container representation.
    test {DEBUG DIGEST for Roaring bitmaps includes trailing zero length} {
        r config set bitmap-default-roaring yes
        r del bitmap:digest:short bitmap:digest:long
        r setbit bitmap:digest:short 3 1
        r setbit bitmap:digest:long 3 1
        r setbit bitmap:digest:long 1024 0
        r config set bitmap-default-roaring no

        assert_equal 1 [r bitcount bitmap:digest:short]
        assert_equal 1 [r bitcount bitmap:digest:long]
        assert_equal 1 [r getbit bitmap:digest:short 3]
        assert_equal 1 [r getbit bitmap:digest:long 3]
        assert {[r debug digest-value bitmap:digest:short] ne [r debug digest-value bitmap:digest:long]}
    }

    test {DEBUG DIGEST for Roaring bitmaps ignores roaring container encoding} {
        r del bitmap:digest:converted bitmap:digest:setbit
        set raw [binary format H* [string repeat ff 1024]]

        r set bitmap:digest:converted $raw
        assert_equal OK [convert_string_bitmap_to_roaring r bitmap:digest:converted]

        r config set bitmap-default-roaring yes
        for {set bit 0} {$bit < 8192} {incr bit} {
            r setbit bitmap:digest:setbit $bit 1
        }
        r config set bitmap-default-roaring no

        assert_equal bitmap [r type bitmap:digest:converted]
        assert_equal bitmap [r type bitmap:digest:setbit]
        assert_equal $raw [r debug bitmap-raw bitmap:digest:converted]
        assert_equal $raw [r debug bitmap-raw bitmap:digest:setbit]
        set converted_digest [r debug digest-value bitmap:digest:converted]
        set setbit_digest [r debug digest-value bitmap:digest:setbit]
        assert_equal $converted_digest $setbit_digest
    }

    test {DEBUG DIGEST visits Roaring set-bit ranges with half-open endpoints} {
        # Exact digests exercise the range visitor through its observable
        # consumer. Cover a singleton, a multi-bit run, and a run merged
        # across CRoaring's 16-bit container boundary.
        foreach {key bits expected_digest} {
            bitmap:digest:ranges:singleton {3}
            {4af788cd0cac52dc5f1a0c491c5afd79ef086b73}
            bitmap:digest:ranges:multi {1 2 3 4}
            {4f213d509203fc746181634c30a9de6b8adba6d0}
            bitmap:digest:ranges:cross-container {65534 65535 65536 65537}
            {252834eaf1e8586068d6bb75c1b6764718fd0a34}
        } {
            assert_equal OK [create_roaring_bitmap_from_bits r $key $bits]
            assert_equal bitmap-roaring [r object encoding $key]
            assert_equal $expected_digest [r debug digest-value $key]
        }
    }

    test {DEBUG DIGEST for fragmented Roaring bitmaps is fast and history independent} {
        # Alternating bits give one set-bit run per two bits, so 1MB holds 4M
        # runs. All of them must be streamed into one SHA1 rather than paying
        # SHA1 finalizations for every run.
        set bytes 1048576
        set raw [string repeat [binary format H* 55] $bytes]
        r del bitmap:digest:frag:converted bitmap:digest:frag:bitfield \
            bitmap:digest:frag:cleared bitmap:digest:frag:converted:restored \
            bitmap:digest:frag:cleared:restored

        assert_equal OK [create_roaring_bitmap_from_raw r bitmap:digest:frag:converted $raw]
        assert_equal [expr {$bytes * 4}] [r bitcount bitmap:digest:frag:converted]
        set digest [r debug digest-value bitmap:digest:frag:converted]
        if {!$::valgrind} {
            # Time the digest against a plain SHA1 pass over a 16MB string in
            # the same server instead of a fixed limit, so the check holds on
            # slow builds and busy runners alike. Streaming costs about 5x that
            # baseline, while a SHA1 finalization per run costs about 100x.
            # Keep the fastest of three runs to ride out scheduling noise.
            r setrange bitmap:digest:baseline [expr {16 * 1024 * 1024 - 1}] x
            foreach key {bitmap:digest:frag:converted bitmap:digest:baseline} {
                set fastest($key) {}
                for {set i 0} {$i < 3} {incr i} {
                    set start [clock milliseconds]
                    r debug digest-value $key
                    set elapsed [expr {[clock milliseconds] - $start}]
                    if {$fastest($key) eq {} || $elapsed < $fastest($key)} {
                        set fastest($key) $elapsed
                    }
                }
            }
            r del bitmap:digest:baseline
            assert_lessthan $fastest(bitmap:digest:frag:converted) \
                [expr {25 * max(1, $fastest(bitmap:digest:baseline))}]
        }

        # Build the same bits through other container histories: BITFIELD
        # writes into an empty bitmap grow array containers into bitsets, and
        # clearing every other bit of an all-ones bitmap fragments its run
        # containers. DUMP/RESTORE then round-trips both representations.
        # The BITFIELD writes loop in Lua to keep them server-side.
        set fill {
            for i = 0, tonumber(ARGV[1]) - 1 do
                redis.call('BITFIELD', KEYS[1], 'SET', 'i64', '#' .. i, ARGV[2])
            end
        }
        set pattern 6148914691236517205 ;# 0x5555555555555555
        r config set bitmap-default-roaring yes
        r eval $fill 1 bitmap:digest:frag:bitfield [expr {$bytes / 8}] $pattern
        r config set bitmap-default-roaring no
        assert_equal OK [create_roaring_bitmap_from_raw r bitmap:digest:frag:cleared \
            [string repeat [binary format H* ff] $bytes]]
        r eval $fill 1 bitmap:digest:frag:cleared [expr {$bytes / 8}] $pattern
        foreach key {bitmap:digest:frag:converted bitmap:digest:frag:cleared} {
            r restore $key:restored 0 [r dump $key]
        }

        foreach key {
            bitmap:digest:frag:bitfield
            bitmap:digest:frag:cleared
            bitmap:digest:frag:converted:restored
            bitmap:digest:frag:cleared:restored
        } {
            assert_equal bitmap-roaring [r object encoding $key]
            assert_equal $raw [r debug bitmap-raw $key]
            assert_equal $digest [r debug digest-value $key]
        }
        r del bitmap:digest:frag:converted bitmap:digest:frag:bitfield \
            bitmap:digest:frag:cleared bitmap:digest:frag:converted:restored \
            bitmap:digest:frag:cleared:restored
    }

    test {Roaring bitmap writes keep the proto-max-bulk-len offset limit} {
        r del bitmap:roaring:bounds
        r config set bitmap-default-roaring yes
        assert_equal 0 [r setbit bitmap:roaring:bounds 0 1]
        r config set bitmap-default-roaring no

        assert_equal bitmap [r type bitmap:roaring:bounds]
        assert_equal 1 [r bitcount bitmap:roaring:bounds]
        assert_equal 0 [r getbit bitmap:roaring:bounds 4294967295]
        assert_equal {0} [r bitfield_ro bitmap:roaring:bounds GET u1 4294967295]
        foreach cmd {
            {getbit bitmap:roaring:bounds 4294967296}
            {setbit bitmap:roaring:bounds 4294967296 1}
            {bitfield bitmap:roaring:bounds SET u1 4294967296 1}
            {bitfield_ro bitmap:roaring:bounds GET u1 4294967296}
            {bitfield bitmap:roaring:bounds GET u1 4294967296 SET u1 0 1}
        } {
            assert_error {*bit offset*out of range*} {r {*}$cmd}
        }
        assert_error {*bit offset*out of range*} {
            r setbit bitmap:roaring:bounds 9223372036854775808 1
        }
        assert_equal 1 [r bitcount bitmap:roaring:bounds]
        r del bitmap:roaring:bounds
    }

    test {bitmap-default-roaring SETBIT rejects out-of-range offsets without changing keys} {
        r config set bitmap-default-roaring yes
        r del bitmap:bounds:implicit:new bitmap:bounds:implicit:string

        assert_error {*bit offset is*out of range*} {
            r setbit bitmap:bounds:implicit:new 4294967296 1
        }
        assert_equal 0 [r exists bitmap:bounds:implicit:new]

        set raw [binary format H* 80]
        r set bitmap:bounds:implicit:string $raw
        assert_error {*bit offset is*out of range*} {
            r setbit bitmap:bounds:implicit:string 4294967296 1
        }
        assert_equal string [r type bitmap:bounds:implicit:string]
        assert_equal $raw [r get bitmap:bounds:implicit:string]
        r config set bitmap-default-roaring no
    }

    test {string bitmaps keep the proto-max-bulk-len offset bound} {
        r del bitmap:string:bounds
        r config set bitmap-default-roaring no
        r set bitmap:string:bounds [binary format H* 80]

        assert_error {*bit offset is*out of range*} {
            r setbit bitmap:string:bounds 4294967296 1
        }
        assert_error {*bit offset is*out of range*} {
            r bitfield bitmap:string:bounds SET u8 4294967296 255
        }
        assert_equal string [r type bitmap:string:bounds]

        assert_error {*bit offset is*out of range*} {
            r getbit bitmap:string:bounds 4294967296
        }
        assert_error {*bit offset is*out of range*} {
            r bitfield_ro bitmap:string:bounds GET u8 4294967296
        }
        assert_error {*bit offset is*out of range*} {
            r bitfield bitmap:string:bounds GET u8 4294967296
        }
        assert_equal 1 [r bitcount bitmap:string:bounds]
    }

    test {bitmap offset limit follows proto-max-bulk-len config} {
        set limit 1048576
        set oldval [config_get_set proto-max-bulk-len $limit]
        set last_allowed [expr {$limit * 8 - 1}]
        set first_rejected [expr {$limit * 8}]

        r del bitmap:roaring:small-limit
        r config set bitmap-default-roaring yes
        assert_equal 0 [r setbit bitmap:roaring:small-limit $last_allowed 1]
        assert_equal 1 [r getbit bitmap:roaring:small-limit $last_allowed]

        foreach cmd [list \
            [list getbit bitmap:roaring:small-limit $first_rejected] \
            [list bitfield_ro bitmap:roaring:small-limit GET u1 $first_rejected] \
            [list setbit bitmap:roaring:small-limit $first_rejected 1] \
            [list bitfield bitmap:roaring:small-limit SET u1 $first_rejected 1] \
            [list bitfield bitmap:roaring:small-limit GET u1 $first_rejected SET u1 0 0] \
        ] {
            assert_error {*bit offset*out of range*} {r {*}$cmd}
        }
        # Like string bitmaps, a BITFIELD write whose offset passes the limit
        # may span up to 63 bits past it.
        assert_equal {2} [r bitfield bitmap:roaring:small-limit SET u2 $last_allowed 3]
        assert_equal 2 [r bitcount bitmap:roaring:small-limit]

        r config set bitmap-default-roaring no
        r config set proto-max-bulk-len $oldval
        r del bitmap:roaring:small-limit
    }

    test {Roaring BITFIELD offsets above UINT32_MAX follow proto-max-bulk-len} {
        set raised_limit [expr {536870912 + 1}]
        set max_roaring_bit 4294967295
        set first_wide_bit [expr {$max_roaring_bit + 1}]
        set oldval [config_get_set proto-max-bulk-len $raised_limit]

        r del bitmap:roaring:raised-limit
        r config set bitmap-default-roaring yes
        assert_equal 0 [r setbit bitmap:roaring:raised-limit 0 1]
        assert_equal {0} [
            r bitfield bitmap:roaring:raised-limit SET u2 $max_roaring_bit 3
        ]
        assert_equal 1 [r getbit bitmap:roaring:raised-limit $max_roaring_bit]
        assert_equal 1 [r getbit bitmap:roaring:raised-limit $first_wide_bit]
        assert_equal {3} [
            r bitfield_ro bitmap:roaring:raised-limit GET u2 $max_roaring_bit
        ]
        assert_equal 3 [r bitcount bitmap:roaring:raised-limit]

        # Lowering the user-configured limit makes the first byte above
        # UINT32_MAX inaccessible while the preceding bit remains readable.
        r config set proto-max-bulk-len 536870912
        assert_equal 1 [r getbit bitmap:roaring:raised-limit $max_roaring_bit]
        assert_error {*bit offset*out of range*} {
            r getbit bitmap:roaring:raised-limit $first_wide_bit
        }

        r config set bitmap-default-roaring no
        r config set proto-max-bulk-len $oldval
        r del bitmap:roaring:raised-limit
    }

    # Conversion is observable through WATCH and ordered type_changed
    # keyspace notifications; these tests pin both behaviors.
    test {WATCH aborts the transaction when bitmap-default-roaring converts the key} {
        # Setting an already set bit is a logical no-op, so only the
        # conversion can touch the watched key. Control: without the
        # conversion the same no-op SETBIT leaves the transaction alone.
        r config set bitmap-default-roaring no
        r del bitmap:public:watch
        r set bitmap:public:watch [binary format H* 80]
        r watch bitmap:public:watch
        assert_equal 1 [r setbit bitmap:public:watch 0 1]
        assert_equal string [r type bitmap:public:watch]
        r multi
        r ping
        assert_equal {PONG} [r exec]

        r config set bitmap-default-roaring yes
        r watch bitmap:public:watch
        assert_equal 1 [r setbit bitmap:public:watch 0 1]
        assert_equal bitmap [r type bitmap:public:watch]
        r multi
        r ping
        assert_equal {} [r exec]
        assert_equal 1 [r bitcount bitmap:public:watch]
        r config set bitmap-default-roaring no
    }

    # Keyspace events are published for the client's selected db, which is
    # db 0 under --singledb and db 9 otherwise.
    set db [expr {$::singledb ? 0 : 9}]

    test {Roaring bitmap creation and conversion emit documented keyspace events in order} {
        r config set bitmap-default-roaring no
        r config set notify-keyspace-events {}
        r del bitmap:public:notify bitmap:public:notify:conv \
            bitmap:public:notify:bitfield bitmap:public:notify:bitfield:fail
        r set bitmap:public:notify:conv [binary format H* 80]
        r set bitmap:public:notify:bitfield [binary format H* 01]
        r set bitmap:public:notify:bitfield:fail [binary format H* ff]

        r config set notify-keyspace-events Eocnb
        set rd [redis_deferring_client]
        $rd psubscribe __keyevent@${db}__:*
        $rd read

        # Direct roaring creation in bitmap-default-roaring yes: same event
        # names as a legacy creating SETBIT ("new" then "setbit"), with the
        # write event classified under the bitmap notification class.
        r config set bitmap-default-roaring yes
        r setbit bitmap:public:notify $sparse_public_offset 1
        assert_equal "pmessage __keyevent@${db}__:* __keyevent@${db}__:new bitmap:public:notify" [$rd read]
        assert_equal "pmessage __keyevent@${db}__:* __keyevent@${db}__:setbit bitmap:public:notify" [$rd read]

        # A no-op SETBIT still converts the representation. BITCONVERT emits
        # type_changed; SETBIT then observes the native value and has no
        # logical write event, exactly as it does during replay.
        assert_equal 1 [r setbit bitmap:public:notify:conv 0 1]
        assert_equal "pmessage __keyevent@${db}__:* __keyevent@${db}__:type_changed bitmap:public:notify:conv" [$rd read]

        # BITFIELD follows the same replay-equivalent contract when its write
        # leaves the logical bits unchanged.
        assert_equal {1} [r bitfield bitmap:public:notify:bitfield SET u8 0 1]
        assert_equal "pmessage __keyevent@${db}__:* __keyevent@${db}__:type_changed bitmap:public:notify:bitfield" [$rd read]

        # The representation transition still occurs when every write is
        # rejected by OVERFLOW FAIL, but the rejected command emits no event.
        assert_equal {{}} [r bitfield bitmap:public:notify:bitfield:fail \
            OVERFLOW FAIL INCRBY u8 0 1]
        assert_equal bitmap [r type bitmap:public:notify:bitfield:fail]
        assert_equal "pmessage __keyevent@${db}__:* __keyevent@${db}__:type_changed bitmap:public:notify:bitfield:fail" [$rd read]
        r config set bitmap-default-roaring no

        $rd close
        r config set notify-keyspace-events {}
    }

    test {Roaring bitmap writes use only the bitmap notification class} {
        r config set bitmap-default-roaring no
        r config set notify-keyspace-events {}
        r del bitmap:notify:roaring-dollar bitmap:notify:string-dollar \
            bitmap:notify:string-bitmap bitmap:notify:roaring-bitmap \
            bitmap:notify:roaring-all bitmap:notify:bitop-source \
            bitmap:notify:bitop-dollar bitmap:notify:bitop-bitmap

        # Seed a roaring source before subscribing so BITOP exercises its own
        # notification call site without adding setup events to the stream.
        r config set bitmap-default-roaring yes
        r setbit bitmap:notify:bitop-source 0 1
        r config set bitmap-default-roaring no

        set rd [redis_deferring_client]
        $rd psubscribe __keyevent@${db}__:*
        $rd read

        r config set notify-keyspace-events E\$
        r config set bitmap-default-roaring yes
        r setbit bitmap:notify:roaring-dollar 0 1
        r config set bitmap-default-roaring no
        assert_equal 1 [r bitop or bitmap:notify:bitop-dollar \
            bitmap:notify:bitop-source]
        # The string SETBIT is a sentinel: if either roaring write above were
        # misclassified as a string event, this read would see it first.
        r setbit bitmap:notify:string-dollar 0 1
        assert_equal "pmessage __keyevent@${db}__:* __keyevent@${db}__:setbit bitmap:notify:string-dollar" [$rd read]

        r config set notify-keyspace-events Eb
        r setbit bitmap:notify:string-bitmap 0 1
        r config set bitmap-default-roaring yes
        r setbit bitmap:notify:roaring-bitmap 0 1
        assert_equal "pmessage __keyevent@${db}__:* __keyevent@${db}__:setbit bitmap:notify:roaring-bitmap" [$rd read]
        assert_equal 1 [r bitop or bitmap:notify:bitop-bitmap \
            bitmap:notify:bitop-source]
        assert_equal "pmessage __keyevent@${db}__:* __keyevent@${db}__:set bitmap:notify:bitop-bitmap" [$rd read]

        r config set notify-keyspace-events EA
        r setbit bitmap:notify:roaring-all 0 1
        assert_equal "pmessage __keyevent@${db}__:* __keyevent@${db}__:setbit bitmap:notify:roaring-all" [$rd read]

        $rd close
        r config set bitmap-default-roaring no
        r config set notify-keyspace-events {}
    }

    # Command-boundary tests verify bitmap commands remain transparent while
    # string-only commands continue to reject native bitmap objects.
    test {public Roaring bitmaps cover the bitmap command surface} {
        r config set bitmap-default-roaring yes

        assert_equal 0 [r setbit bitmap:public:commands $sparse_public_offset 1]
        assert_equal 1 [r getbit bitmap:public:commands $sparse_public_offset]
        assert_equal 1 [r bitcount bitmap:public:commands]
        assert_equal $sparse_public_offset [r bitpos bitmap:public:commands 1]
        assert_equal [list 1] [r bitfield_ro bitmap:public:commands GET u1 $sparse_public_offset]

        set next_offset [expr {$sparse_public_offset + 1}]
        assert_equal [list 0] [r bitfield bitmap:public:commands SET u1 $next_offset 1]
        assert_equal bitmap [r type bitmap:public:commands]
        assert_equal 2 [r bitcount bitmap:public:commands]
        assert_equal $sparse_public_len [r bitop or bitmap:public:commands:copy bitmap:public:commands]
        assert_equal bitmap [r type bitmap:public:commands:copy]
        assert_equal 2 [r bitcount bitmap:public:commands:copy]
        r config set bitmap-default-roaring no
    }

    test {BITOP destination follows the source types with bitmap-default-roaring no} {
        r config set bitmap-default-roaring no
        r del bitop:dest:s1 bitop:dest:s2 bitop:dest:n1 bitop:dest:out

        r set bitop:dest:s1 [binary format H* f0]
        r set bitop:dest:s2 [binary format H* 0f]

        # All-string sources keep producing a string destination.
        assert_equal 1 [r bitop or bitop:dest:out bitop:dest:s1 bitop:dest:s2]
        assert_equal string [r type bitop:dest:out]
        assert_equal [binary format H* ff] [r get bitop:dest:out]

        # One roaring source makes the destination roaring, even overwriting
        # the previous string destination.
        r set bitop:dest:n1 [binary format H* f0]
        convert_string_bitmap_to_roaring r bitop:dest:n1
        assert_equal 1 [r bitop or bitop:dest:out bitop:dest:n1 bitop:dest:s2]
        assert_equal bitmap [r type bitop:dest:out]
        assert_equal [binary format H* ff] [r debug bitmap-raw bitop:dest:out]
    }

    test {BITOP uses Roaring for nonempty results with bitmap-default-roaring yes} {
        r config set bitmap-default-roaring no
        r del bitop:imp:s1 bitop:imp:s2 bitop:imp:out \
            bitop:imp:zero bitop:imp:empty bitop:imp:missing
        r set bitop:imp:s1 [binary format H* cc]
        r set bitop:imp:s2 [binary format H* aa]

        r config set bitmap-default-roaring yes
        assert_equal 1 [r bitop xor bitop:imp:out bitop:imp:s1 bitop:imp:s2]
        assert_equal bitmap [r type bitop:imp:out]
        assert_equal [binary format H* 66] [r debug bitmap-raw bitop:imp:out]

        # A nonempty all-zero result keeps its logical byte length and native
        # representation during replay.
        assert_equal 1 [r bitop xor bitop:imp:zero bitop:imp:s1 bitop:imp:s1]
        assert_equal bitmap [r type bitop:imp:zero]
        assert_equal [binary format H* 00] [r debug bitmap-raw bitop:imp:zero]

        # Destination/source aliasing remains valid when replay needs the
        # native result to be selected explicitly.
        assert_equal 1 [r bitop not bitop:imp:s1 bitop:imp:s1]
        assert_equal bitmap [r type bitop:imp:s1]
        assert_equal [binary format H* 33] [r debug bitmap-raw bitop:imp:s1]

        # An empty logical result deletes the destination, and replay must not
        # recreate an empty bitmap.
        r set bitop:imp:empty keep
        assert_equal 0 [r bitop or bitop:imp:empty bitop:imp:missing]
        assert_equal none [r type bitop:imp:empty]
        r config set bitmap-default-roaring no
    }

    test {BITOP NOT allows oversized string sources when destination would be roaring} {
        set limit 1048576
        set oldval [config_get_set proto-max-bulk-len [expr {$limit + 1}]]
        r config set bitmap-default-roaring no
        r del bitop:not:mixed:big bitop:not:mixed:out
        r setbit bitop:not:mixed:big [expr {($limit + 1) * 8 - 1}] 1
        r config set proto-max-bulk-len $limit
        r config set bitmap-default-roaring yes

        assert_equal [expr {$limit + 1}] [r bitop not bitop:not:mixed:out bitop:not:mixed:big]
        assert_equal bitmap [r type bitop:not:mixed:out]
        assert_equal 1 [r getbit bitop:not:mixed:out 0]
        assert_equal string [r type bitop:not:mixed:big]

        r config set bitmap-default-roaring no
        r config set proto-max-bulk-len $oldval
        # Restore the limit before reading the high bit (GETBIT caps its offset
        # at proto-max-bulk-len too).
        assert_equal 0 [r getbit bitop:not:mixed:out [expr {($limit + 1) * 8 - 1}]]
        r del bitop:not:mixed:big bitop:not:mixed:out
    }

    test {BITOP with sparse Roaring sources computes in Roaring space} {
        r del bitop:sparse:a bitop:sparse:b bitop:sparse:out
        r config set bitmap-default-roaring yes
        r setbit bitop:sparse:a 131071 1
        r setbit bitop:sparse:a 5 1
        r setbit bitop:sparse:b 131071 1
        r config set bitmap-default-roaring no

        assert_equal 16384 [r bitop xor bitop:sparse:out bitop:sparse:a bitop:sparse:b]
        assert_equal bitmap [r type bitop:sparse:out]
        assert_equal 1 [r bitcount bitop:sparse:out]
        assert_equal 5 [r bitpos bitop:sparse:out 1]

        assert_equal 16384 [r bitop and bitop:sparse:out bitop:sparse:a bitop:sparse:b]
        assert_equal 1 [r bitcount bitop:sparse:out]
        assert_equal 131071 [r bitpos bitop:sparse:out 1]
        r del bitop:sparse:a bitop:sparse:b bitop:sparse:out
    }

    test {Roaring bitmap helper exposes type encoding and exact raw bytes} {
        set raw [binary format H* 80400100080000]

        r set bitmap:raw $raw
        assert_equal [convert_string_bitmap_to_roaring r bitmap:raw] OK
        assert_equal [r type bitmap:raw] bitmap
        assert_equal [r object encoding bitmap:raw] bitmap-roaring
        assert_equal [r debug bitmap-raw bitmap:raw] $raw
        assert_error {WRONGTYPE*} {r get bitmap:raw}
    }

    test {Roaring bitmap scan type and copy preserve bitmap objects} {
        set raw [binary format H* 010204000000]

        r set bitmap:copy-source $raw
        r set bitmap:string-peer value
        convert_string_bitmap_to_roaring r bitmap:copy-source
        # SCAN gives no guarantee that one page covers the keyspace, so walk
        # the cursor to completion before asserting membership.
        set keys {}
        set cursor 0
        while 1 {
            set scan_reply [r scan $cursor type bitmap]
            set cursor [lindex $scan_reply 0]
            foreach key [lindex $scan_reply 1] { lappend keys $key }
            if {$cursor == 0} break
        }
        assert {[lsearch -exact $keys bitmap:copy-source] >= 0}
        assert {[lsearch -exact $keys bitmap:string-peer] == -1}

        assert_equal [r copy bitmap:copy-source bitmap:copy-target] 1
        assert_equal [r type bitmap:copy-target] bitmap
        assert_equal [r object encoding bitmap:copy-target] bitmap-roaring
        assert_equal [r debug bitmap-raw bitmap:copy-target] $raw
    }

    test {Roaring bitmap rejects generic string commands without materializing} {
        set raw [binary format H* 80400100080000]

        r set bitmap:string-boundary $raw
        convert_string_bitmap_to_roaring r bitmap:string-boundary
        foreach command {
            {get bitmap:string-boundary}
            {getex bitmap:string-boundary}
            {getdel bitmap:string-boundary}
            {getset bitmap:string-boundary replacement}
            {strlen bitmap:string-boundary}
            {getrange bitmap:string-boundary 0 -1}
            {setrange bitmap:string-boundary 0 x}
            {append bitmap:string-boundary x}
            {incr bitmap:string-boundary}
            {decr bitmap:string-boundary}
            {incrby bitmap:string-boundary 2}
            {decrby bitmap:string-boundary 2}
            {incrbyfloat bitmap:string-boundary 1.25}
            {set bitmap:string-boundary replacement get}
        } {
            assert_error {WRONGTYPE*} {r {*}$command}
            assert_equal bitmap [r type bitmap:string-boundary]
            assert_equal bitmap-roaring [r object encoding bitmap:string-boundary]
            assert_equal $raw [r debug bitmap-raw bitmap:string-boundary]
        }
    }

    test {legacy string bitmaps keep normal string command behavior} {
        set raw [binary format H* 8040]

        r set bitmap:legacy-boundary $raw
        assert_equal string [r type bitmap:legacy-boundary]
        assert_equal $raw [r get bitmap:legacy-boundary]
        assert_equal 2 [r strlen bitmap:legacy-boundary]
        assert_equal 2 [r bitcount bitmap:legacy-boundary]
        assert_equal 1 [r setbit bitmap:legacy-boundary 0 0]
        assert_equal string [r type bitmap:legacy-boundary]
        assert_equal [binary format H* 0040] [r get bitmap:legacy-boundary]
        assert_equal 3 [r append bitmap:legacy-boundary x]
        assert_equal [binary format H* 004078] [r get bitmap:legacy-boundary]
    }

    test {plain SET overwrites a Roaring bitmap key with a string} {
        set raw [binary format H* 80400100080000]

        r set bitmap:set-overwrite $raw
        convert_string_bitmap_to_roaring r bitmap:set-overwrite
        assert_equal bitmap [r type bitmap:set-overwrite]

        # Generic overwrite is the intended plain replacement path: SET
        # replaces a Roaring bitmap like it replaces any other type, while
        # implicit string reads stay WRONGTYPE.
        r set bitmap:set-overwrite replacement
        assert_equal string [r type bitmap:set-overwrite]
        assert_equal replacement [r get bitmap:set-overwrite]
    }

    test {existence-conditional writes treat Roaring bitmaps as existing keys} {
        set raw [binary format H* 80400100080000]

        r del bitmap:nx-boundary bitmap:nx-other
        r set bitmap:nx-boundary $raw
        convert_string_bitmap_to_roaring r bitmap:nx-boundary
        # NX-style writes check only existence, never type: a Roaring bitmap
        # counts as existing and stays untouched.
        assert_equal 0 [r setnx bitmap:nx-boundary value]
        assert_equal 0 [r msetnx bitmap:nx-boundary value bitmap:nx-other other]
        assert_equal 0 [r exists bitmap:nx-other]
        assert_equal bitmap [r type bitmap:nx-boundary]
        assert_equal $raw [r debug bitmap-raw bitmap:nx-boundary]

        # SET ... XX overwrites a Roaring bitmap like plain SET does.
        assert_equal OK [r set bitmap:nx-boundary replacement xx]
        assert_equal string [r type bitmap:nx-boundary]
        assert_equal replacement [r get bitmap:nx-boundary]
    }

    test {Roaring bitmap stays opaque to additional string read surfaces} {
        set raw [binary format H* 80400100080000]

        r set bitmap:surface $raw
        r set bitmap:surface:string $raw
        convert_string_bitmap_to_roaring r bitmap:surface
        # MGET reports non-string keys as nil, Roaring bitmaps included.
        assert_equal [list {} $raw] [r mget bitmap:surface bitmap:surface:string]
        # SUBSTR is the legacy alias of GETRANGE and stays WRONGTYPE.
        assert_error {WRONGTYPE*} {r substr bitmap:surface 0 -1}
        # LCS refuses non-string keys with its dedicated error.
        assert_error {*must contain string values*} {r lcs bitmap:surface bitmap:surface:string}

        assert_equal bitmap [r type bitmap:surface]
        assert_equal $raw [r debug bitmap-raw bitmap:surface]
    }

    test {SORT BY and GET patterns treat Roaring bitmaps as missing values} {
        r del bitmap:sort:list
        r rpush bitmap:sort:list a b
        r set weight_a 2
        r set weight_b 1
        r set data_a string-a
        r set data_b string-b

        assert_equal {b a} [r sort bitmap:sort:list BY weight_* GET #]
        assert_equal {string-b string-a} [r sort bitmap:sort:list BY weight_* GET data_*]

        # lookupKeyByPattern() only dereferences OBJ_STRING values, so a
        # Roaring bitmap weight or data target behaves exactly like a
        # missing key: no weight for BY (sorts as 0), nil for GET, and no
        # materialization back to a string.
        convert_string_bitmap_to_roaring r weight_a
        convert_string_bitmap_to_roaring r data_a
        assert_equal {a b} [r sort bitmap:sort:list BY weight_* GET #]
        assert_equal [list {} string-b] [r sort bitmap:sort:list BY weight_* GET data_*]
        assert_equal bitmap [r type weight_a]
        assert_equal bitmap [r type data_a]

        # The hash-field pattern branch ("BY pat->field") takes a separate
        # lookup path that requires OBJ_HASH; a Roaring bitmap in pattern
        # position behaves like a missing key there too.
        r del wh_a wh_b
        r hset wh_b f 1
        r set wh_a placeholder
        convert_string_bitmap_to_roaring r wh_a
        assert_equal {a b} [r sort bitmap:sort:list BY wh_*->f GET #]
        assert_equal [list {} 1] [r sort bitmap:sort:list BY wh_*->f GET wh_*->f]
        assert_equal bitmap [r type wh_a]
    }

    test {Lua scripts observe Roaring bitmaps through normal type checks} {
        set raw [binary format H* 80400100080000]

        r set bitmap:lua $raw
        convert_string_bitmap_to_roaring r bitmap:lua
        assert_equal 1 [r eval {return redis.call('getbit', KEYS[1], 0)} 1 bitmap:lua]
        assert_equal 4 [r eval {return redis.call('bitcount', KEYS[1])} 1 bitmap:lua]
        assert_error {*WRONGTYPE*} {r eval {return redis.call('get', KEYS[1])} 1 bitmap:lua}
        assert_equal bitmap [r type bitmap:lua]
    }

    # Persistence tests cover logical length, portable wire bytes, corruption
    # rejection, and sparse high-offset behavior separately.
    test {Roaring bitmap dump restore and debug reload preserve bitmap objects} {
        set raw [binary format H* f0000000000000010000]

        r set bitmap:persist $raw
        convert_string_bitmap_to_roaring r bitmap:persist
        set payload [r dump bitmap:persist]
        r restore bitmap:restored 0 $payload
        assert_equal [r type bitmap:restored] bitmap
        assert_equal [r object encoding bitmap:restored] bitmap-roaring
        assert_equal [r debug bitmap-raw bitmap:restored] $raw

        r debug reload
        assert_equal [r type bitmap:persist] bitmap
        assert_equal [r object encoding bitmap:persist] bitmap-roaring
        assert_equal [r debug bitmap-raw bitmap:persist] $raw
        assert_equal [r type bitmap:restored] bitmap
        assert_equal [r debug bitmap-raw bitmap:restored] $raw
    }

    test {RESTORE REPLACE preserves explicit string and Roaring bitmap transitions} {
        set raw [binary format H* 80400100080000]

        r del bitmap:restore:source bitmap:restore:target
        r set bitmap:restore:source $raw
        set string_payload [r dump bitmap:restore:source]

        convert_string_bitmap_to_roaring r bitmap:restore:source
        set bitmap_payload [r dump bitmap:restore:source]
        assert_equal bitmap [r type bitmap:restore:source]
        assert_equal bitmap-roaring [r object encoding bitmap:restore:source]

        r restore bitmap:restore:target 0 $string_payload
        assert_equal string [r type bitmap:restore:target]
        assert_equal $raw [r get bitmap:restore:target]

        r restore bitmap:restore:target 0 $bitmap_payload replace
        assert_equal bitmap [r type bitmap:restore:target]
        assert_equal bitmap-roaring [r object encoding bitmap:restore:target]
        assert_equal $raw [r debug bitmap-raw bitmap:restore:target]

        r restore bitmap:restore:target 0 $string_payload replace
        assert_equal string [r type bitmap:restore:target]
        assert_equal $raw [r get bitmap:restore:target]
    }

    test {Roaring bitmap portable RDB restores run containers without capacity bloat} {
        set raw ""
        for {set i 0} {$i < 32} {incr i} {
            append raw [string repeat [binary format H* ff] 600]
            append raw [string repeat [binary format H* 00] 7592]
        }

        r del bitmap:rdb-run:a bitmap:rdb-run:b
        r set bitmap:rdb-run:a $raw
        convert_string_bitmap_to_roaring r bitmap:rdb-run:a
        set original_usage [r memory usage bitmap:rdb-run:a]

        r restore bitmap:rdb-run:b 0 [r dump bitmap:rdb-run:a]
        assert_equal bitmap [r type bitmap:rdb-run:b]
        assert_equal $raw [r debug bitmap-raw bitmap:rdb-run:b]

        set restored_usage [r memory usage bitmap:rdb-run:b]
        assert_lessthan_equal $restored_usage [expr {$original_usage + 8192}] \
            "restored_usage=$restored_usage original_usage=$original_usage"
        r del bitmap:rdb-run:a bitmap:rdb-run:b
    }

    test {Roaring bitmap RESTORE and RDB load compact SETBIT-built runs} {
        set key bitmap:rdb-compact:setbit
        set restored bitmap:rdb-compact:restored
        r del $key $restored

        # SETBIT never compacts containers, so 70000 consecutive bits stay in
        # two BITSET containers: one full chunk and one above the ARRAY limit.
        r config set bitmap-default-roaring yes
        r eval {
            for i = 0, tonumber(ARGV[1]) - 1 do
                redis.call('setbit', KEYS[1], i, 1)
            end
        } 1 $key 70000
        r config set bitmap-default-roaring no
        assert_equal bitmap [r type $key]
        assert_equal 70000 [r bitcount $key]
        set digest [r debug digest-value $key]
        set setbit_usage [r memory usage $key]
        assert_morethan $setbit_usage 16384

        # Loading the portable payload rewrites both chunks as RUN containers.
        r restore $restored 0 [r dump $key]
        set restored_usage [r memory usage $restored]
        assert_lessthan $restored_usage 1024 \
            "restored_usage=$restored_usage setbit_usage=$setbit_usage"
        assert_equal 70000 [r bitcount $restored]
        assert_equal $digest [r debug digest-value $restored]

        r debug reload
        foreach k [list $key $restored] {
            set reloaded_usage [r memory usage $k]
            assert_lessthan $reloaded_usage 1024 \
                "key=$k reloaded_usage=$reloaded_usage setbit_usage=$setbit_usage"
            assert_equal bitmap-roaring [r object encoding $k]
            assert_equal 70000 [r bitcount $k]
            assert_equal $digest [r debug digest-value $k]
        }
        r del $key $restored
    }

    test {Roaring bitmap RDB uses compact payload for fragmented bitmaps} {
        set raw [string repeat [binary format H* 55] 8192]

        r del bitmap:rdb-frag:a bitmap:rdb-frag:b
        r set bitmap:rdb-frag:a $raw
        convert_string_bitmap_to_roaring r bitmap:rdb-frag:a
        set dump [r dump bitmap:rdb-frag:a]
        assert_lessthan [string length $dump] [expr {[string length $raw] + 128}] \
            "dump_len=[string length $dump] raw_len=[string length $raw]"

        r restore bitmap:rdb-frag:b 0 $dump
        assert_equal bitmap [r type bitmap:rdb-frag:b]
        assert_equal $raw [r debug bitmap-raw bitmap:rdb-frag:b]
        r del bitmap:rdb-frag:a bitmap:rdb-frag:b
    }

    test {Roaring bitmap portable RDB payload keeps sparse bitmaps compact} {
        set oldcomp [config_get_set rdbcompression yes]

        r del bitmap:rdb-sparse:string bitmap:rdb-sparse:roaring \
            bitmap:rdb-sparse:restored
        for {set i 0} {$i < 4096} {incr i} {
            r setbit bitmap:rdb-sparse:string [expr {$i * 4096}] 1
        }
        set raw [r get bitmap:rdb-sparse:string]
        r set bitmap:rdb-sparse:roaring $raw
        convert_string_bitmap_to_roaring r bitmap:rdb-sparse:roaring
        set roaring_dump [r dump bitmap:rdb-sparse:roaring]
        assert_lessthan [string length $roaring_dump] \
            [expr {[string length $raw] / 8}] \
            "roaring_dump_len=[string length $roaring_dump] raw_len=[string length $raw]"

        r restore bitmap:rdb-sparse:restored 0 $roaring_dump
        assert_equal bitmap [r type bitmap:rdb-sparse:restored]
        assert_equal bitmap-roaring [r object encoding bitmap:rdb-sparse:restored]
        assert_equal $raw [r debug bitmap-raw bitmap:rdb-sparse:restored]

        r del bitmap:rdb-sparse:string bitmap:rdb-sparse:roaring \
            bitmap:rdb-sparse:restored
        r config set rdbcompression $oldcomp
    }

    test {Roaring bitmap portable RDB payload round-trips across internal shapes} {
        set dense [string repeat [binary format H* ff] 8192]

        set trailing_zero [binary format H* 80]
        append trailing_zero [string repeat [binary format H* 00] 1023]

        # Build one mixed bitmap holding all three internal container kinds so
        # the RDB round-trip rehydrates them from observable bitmap data.
        # Each 65536-bit chunk is 8192 bytes:
        # chunk 0: 4800 consecutive set bits -> run container
        # chunk 1: alternating bits, cardinality 8000 -> bitset container
        # chunk 2: 64 isolated bits -> array container
        # chunk 3: another run container, lifting the container count to the
        #          CRoaring offset-header threshold so the offsets section is
        #          present alongside the run bitmap.
        set mixed [string repeat [binary format H* ff] 600]
        append mixed [string repeat [binary format H* 00] 7592]
        append mixed [string repeat [binary format H* aa] 2000]
        append mixed [string repeat [binary format H* 00] 6192]
        for {set i 0} {$i < 64} {incr i} {
            append mixed [binary format H* 80][string repeat [binary format H* 00] 15]
        }
        append mixed [string repeat [binary format H* 00] 7168]
        append mixed [string repeat [binary format H* ff] 600]

        # An array-only bitmap spanning two containers keeps sparse data valid
        # across more than one high48 bucket.
        set sparse [binary format H* 80]
        append sparse [string repeat [binary format H* 00] 8191]
        append sparse [binary format H* 80]

        foreach {name raw} [list dense $dense trailing-zero $trailing_zero mixed $mixed sparse $sparse] {
            r set bitmap:endian:$name $raw
            convert_string_bitmap_to_roaring r bitmap:endian:$name
            assert_equal [r debug bitmap-raw bitmap:endian:$name] $raw

            r restore bitmap:endian:restored:$name 0 [r dump bitmap:endian:$name]
            assert_equal bitmap [r type bitmap:endian:restored:$name]
            assert_equal [r debug bitmap-raw bitmap:endian:restored:$name] $raw
        }
    }

    test {Roaring bitmap RDB uses the canonical little-endian portable format} {
        set old_compression [config_get_set rdbcompression no]
        r del bitmap:rdb:portable-wire
        r config set bitmap-default-roaring yes
        r setbit bitmap:rdb:portable-wire 0 1
        r config set bitmap-default-roaring no

        # The portable fields are all little-endian, independent of the host
        # architecture. Keep this fixture independent of the Redis RDB
        # version and checksum surrounding the blob.
        binary scan [roaring_portable_payload \
            [r dump bitmap:rdb:portable-wire]] H* wire
        assert_equal \
            0100000000000000000000003a3000000100000000000000100000000000 \
            $wire
        r config set rdbcompression $old_compression
    }

    test {Roaring portable RDB fields have canonical byte order across container types} {
        set old_compression [config_get_set rdbcompression no]

        # A run container covers its cookie, cardinality, run count, start,
        # and length fields with non-palindromic values.
        set run_raw [string repeat [binary format H* 00] 576]
        append run_raw [string repeat [binary format H* ff] 32]
        r set bitmap:rdb:wire-run $run_raw
        convert_string_bitmap_to_roaring r bitmap:rdb:wire-run
        binary scan [roaring_portable_payload \
            [r dump bitmap:rdb:wire-run]] H* run_wire
        assert_equal \
            0100000000000000000000003b300000010000ff0001000012ff00 \
            $run_wire

        # Force a bitset container and check its multi-byte cardinality,
        # offset, and first 64-bit word without embedding the full 8 KiB blob.
        r set bitmap:rdb:wire-bitset [string repeat [binary format H* aa] 8192]
        convert_string_bitmap_to_roaring r bitmap:rdb:wire-bitset
        set bitset_blob [roaring_portable_payload \
            [r dump bitmap:rdb:wire-bitset]]
        binary scan [string range $bitset_blob 0 35] H* bitset_prefix
        assert_equal \
            0100000000000000000000003a300000010000000000ff7f100000005555555555555555 \
            $bitset_prefix

        # Use two high-32 buckets, a nonzero container key, and nonzero uint16
        # array values so both the 64-bit extension and array byte order are
        # covered. This logical length remains valid on 32-bit builds.
        set high_bit [expr {(1 << 32) + 0x16000}]
        set byte_len [expr {($high_bit >> 3) + 1}]
        set old_limit [config_get_set proto-max-bulk-len $byte_len]
        create_roaring_bitmap_from_bits r bitmap:rdb:wire-array \
            [list [expr {0x1234}] $high_bit]
        binary scan [roaring_portable_payload \
            [r dump bitmap:rdb:wire-array]] H* array_wire
        assert_equal \
            0200000000000000000000003a3000000100000000000000100000003412010000003a3000000100000001000000100000000060 \
            $array_wire
        r config set proto-max-bulk-len $old_limit

        r config set rdbcompression $old_compression
    }

    test {Roaring bitmap RDB rejects invalid portable payloads} {
        set one_bit 0100000000000000000000003a3000000100000000000000100000000000
        set checksum 0000000000000000

        # The blob sets bit 0, which cannot fit a zero-byte logical length.
        set invalid_len [binary format H* "21001e${one_bit}1000${checksum}"]
        assert_error {*Bad data format*} {
            r restore bitmap:rdb:invalid-len 0 $invalid_len
        }

        # The loader rejects a logical byte length beyond the public bitmap
        # limit before the object can reach bitmap operations. 0x81 is the RDB
        # 64-bit length marker and the following value is 2^60, one byte past
        # BITROAR_MAX_BYTES. The blob is valid, so only the bound rejects it.
        set oversized_len [binary format H* "218110000000000000001e${one_bit}1000${checksum}"]
        assert_error {*Bad data format*} {
            r restore bitmap:rdb:oversized-len 0 $oversized_len
        }

        # The same blob at exactly BITROAR_MAX_BYTES (2^60-1) is accepted, so
        # the rejection above comes from the bound. 32-bit builds reject any
        # length beyond SIZE_MAX.
        if {[s arch_bits] == 64} {
            set max_len [binary format H* "21810fffffffffffffff1e${one_bit}1000${checksum}"]
            r restore bitmap:rdb:max-len 0 $max_len
            assert_equal bitmap [r type bitmap:rdb:max-len]
            assert_equal 1 [r bitcount bitmap:rdb:max-len]
            r del bitmap:rdb:max-len
        }

        # A valid portable bitmap must consume the entire RDB string payload.
        set trailing [binary format H* "21011f${one_bit}001000${checksum}"]
        assert_error {*Bad data format*} {
            r restore bitmap:rdb:trailing 0 $trailing
        }
        assert_equal 0 [r exists bitmap:rdb:invalid-len \
            bitmap:rdb:oversized-len bitmap:rdb:trailing]
    }

    test {Roaring bitmap RESTORE rejects truncated payloads} {
        set old_compression [config_get_set rdbcompression no]

        # Six array containers make the portable blob longer than 63 bytes, so
        # both the logical byte length and the blob length use multi-byte RDB
        # length encodings.
        create_roaring_bitmap_from_bits r bitmap:rdb:truncated \
            {3 70000 140000 210000 280000 1000000}
        set dump [r dump bitmap:rdb:truncated]
        r del bitmap:rdb:truncated
        r config set rdbcompression $old_compression

        # Cut the value at every offset, from the logical byte length through
        # the portable blob, and append the RDB version and an all-zero
        # checksum. RESTORE hands that footer to the loader as well, so each
        # cut drops more than ten bytes. A shorter cut could let the footer
        # bytes complete the blob as a different, valid bitmap.
        set body [string range $dump 0 end-10]
        set footer [string range $dump end-9 end-8][binary format x8]
        for {set len 1} {$len < [string length $body] - 10} {incr len} {
            set truncated [string range $body 0 [expr {$len - 1}]]$footer
            assert_error {*Bad data format*} {
                r restore bitmap:rdb:truncated 0 $truncated
            }
        }
        assert_equal 0 [r exists bitmap:rdb:truncated]
    }

    if {[s arch_bits] == 64} {
        test {Roaring bitmap DUMP stays compact at a 2^40 bit offset} {
            set high_bit [expr {(1 << 40) - 1}]
            set byte_len [expr {($high_bit >> 3) + 1}]
            set old_limit [config_get_set proto-max-bulk-len $byte_len]

            r config set bitmap-default-roaring yes
            r del bitmap:rdb:high bitmap:rdb:high:restored
            assert_equal 0 [r setbit bitmap:rdb:high $high_bit 1]
            r config set bitmap-default-roaring no

            r config set proto-max-bulk-len 1048576
            set payload [r dump bitmap:rdb:high]
            assert_lessthan [string length $payload] 256
            assert_error {*bitmap length exceeds proto-max-bulk-len*} {
                r debug bitmap-raw bitmap:rdb:high
            }
            r restore bitmap:rdb:high:restored 0 $payload
            assert_equal bitmap [r type bitmap:rdb:high:restored]
            assert_lessthan [r memory usage bitmap:rdb:high:restored] 65536

            r config set proto-max-bulk-len $byte_len
            assert_equal 1 [r getbit bitmap:rdb:high:restored $high_bit]
            assert_equal 1 [r bitcount bitmap:rdb:high:restored]

            r del bitmap:rdb:high bitmap:rdb:high:restored
            r config set proto-max-bulk-len $old_limit
        }
    }

    test {Roaring bitmap RDB save is not bounded by current proto-max-bulk-len} {
        set limit 1048576
        set byte_len [expr {$limit + 1}]
        set oldval [config_get_set proto-max-bulk-len [expr {$byte_len + 1024}]]
        set raw [string repeat [binary format H* 8000] [expr {$limit / 2}]]
        append raw [binary format H* 80]
        assert_equal $byte_len [string length $raw]

        r del bitmap:rdb-raw-bulk-limit
        r set bitmap:rdb-raw-bulk-limit $raw
        convert_string_bitmap_to_roaring r bitmap:rdb-raw-bulk-limit
        r config set proto-max-bulk-len $limit

        r debug reload

        r config set proto-max-bulk-len [expr {$byte_len + 1024}]
        assert_equal bitmap [r type bitmap:rdb-raw-bulk-limit]
        assert_equal bitmap-roaring [r object encoding bitmap:rdb-raw-bulk-limit]
        assert_equal $raw [r debug bitmap-raw bitmap:rdb-raw-bulk-limit]
        r del bitmap:rdb-raw-bulk-limit
        r config set proto-max-bulk-len $oldval
    }

    test {Roaring bitmap RDB load is not bounded by current proto-max-bulk-len} {
        set limit 1048576
        set oldval [config_get_set proto-max-bulk-len [expr {$limit + 1}]]
        r config set bitmap-default-roaring yes

        set offset [expr {$limit * 8}]
        r setbit bitmap:rdb:above-bulk-limit $offset 1
        r config set proto-max-bulk-len $limit

        r debug reload

        assert_equal bitmap [r type bitmap:rdb:above-bulk-limit]
        assert_equal bitmap-roaring [r object encoding bitmap:rdb:above-bulk-limit]
        r config set proto-max-bulk-len [expr {$limit + 1}]
        assert_equal 1 [r getbit bitmap:rdb:above-bulk-limit $offset]

        r config set bitmap-default-roaring no
        r config set proto-max-bulk-len $oldval
    }

    test {Roaring bitmap lazyfree preserves the container-count threshold} {
        r config resetstat
        r config set bitmap-default-roaring yes

        # 31 containers have an estimated effort of exactly 64, so freeing
        # remains synchronous at the strict greater-than threshold.
        for {set i 0} {$i < 31} {incr i} {
            r setbit bitmap:lazy:sync [expr {$i * 65536}] 1
        }
        assert_equal bitmap [r type bitmap:lazy:sync]
        assert_equal 1 [r unlink bitmap:lazy:sync]
        assert_equal 0 [s lazyfree_pending_objects]
        assert_equal 0 [s lazyfreed_objects]

        # The 32nd container raises the estimated effort to 66 and queues the
        # object for lazyfree. The bounded count must still observe this edge.
        for {set i 0} {$i < 32} {incr i} {
            r setbit bitmap:lazy:async [expr {$i * 65536}] 1
        }
        r config set bitmap-default-roaring no
        assert_equal bitmap [r type bitmap:lazy:async]

        assert_equal 1 [r unlink bitmap:lazy:async]
        wait_for_condition 50 100 {
            [s lazyfree_pending_objects] == 0
        } else {
            fail "lazyfree isn't done"
        }
        assert_equal [s lazyfreed_objects] 1
    } {} {needs:config-resetstat}

    test {public-created Roaring bitmaps survive debug reload} {
        r config set bitmap-default-roaring yes

        r setbit bitmap:public:reload:direct $sparse_public_offset 1
        r set bitmap:public:reload:auto ""
        r setbit bitmap:public:reload:auto $sparse_public_offset 1
        assert {[string length [r dump bitmap:public:reload:direct]] < 256}
        set digest_before [debug_digest]

        r debug reload

        assert_equal [debug_digest] $digest_before
        assert_equal bitmap [r type bitmap:public:reload:direct]
        assert_equal bitmap [r type bitmap:public:reload:auto]
        assert_equal 1 [r getbit bitmap:public:reload:direct $sparse_public_offset]
        assert_equal 1 [r getbit bitmap:public:reload:auto $sparse_public_offset]
        r config set bitmap-default-roaring no
    }
}

start_server {tags {"bitmap" "bitmap-roaring" "needs:debug" "external:skip" "cluster:skip" "logreqres:skip"} overrides {save {} aof-use-rdb-preamble no}} {
    test {Roaring bitmap survives AOF rewrite as bitmap} {
        r config set appendonly yes
        r config set auto-aof-rewrite-percentage 0
        waitForBgrewriteaof r

        set raw [binary format H* 80000000000000000001]

        r set bitmap:aof $raw
        convert_string_bitmap_to_roaring r bitmap:aof
        set digest_before [debug_digest]

        r bgrewriteaof
        waitForBgrewriteaof r
        r debug loadaof

        assert_equal [debug_digest] $digest_before
        assert_equal [r type bitmap:aof] bitmap
        assert_equal [r object encoding bitmap:aof] bitmap-roaring
        assert_equal [r debug bitmap-raw bitmap:aof] $raw
    }

    test {public-created Roaring bitmaps survive AOF rewrite as bitmap} {
        r flushall
        r config set appendonly yes
        waitForBgrewriteaof r
        r config set auto-aof-rewrite-percentage 0
        r config set bitmap-default-roaring yes

        r setbit bitmap:public:aof:direct $sparse_public_offset 1
        r setbit bitmap:public:aof:zero 0 0
        r set bitmap:public:aof:auto ""
        r setbit bitmap:public:aof:auto $sparse_public_offset 1

        set test_high_offset [expr {[s arch_bits] == 64}]
        if {$test_high_offset} {
            set high_bit [expr {(1 << 40) - 1}]
            set byte_len [expr {($high_bit >> 3) + 1}]
            set old_limit [config_get_set proto-max-bulk-len $byte_len]
            r setbit bitmap:public:aof:high $high_bit 1
            r config set proto-max-bulk-len 1048576
            assert_lessthan [string length [r dump bitmap:public:aof:high]] 256
        }
        set digest_before [debug_digest]

        r bgrewriteaof
        waitForBgrewriteaof r
        r debug loadaof

        assert_equal [debug_digest] $digest_before
        assert_equal bitmap [r type bitmap:public:aof:direct]
        assert_equal bitmap [r type bitmap:public:aof:zero]
        assert_equal bitmap [r type bitmap:public:aof:auto]
        if {$test_high_offset} {
            assert_equal bitmap [r type bitmap:public:aof:high]
        }
        assert_equal 1 [r getbit bitmap:public:aof:direct $sparse_public_offset]
        assert_equal [binary format H* 00] [r debug bitmap-raw bitmap:public:aof:zero]
        assert_equal 1 [r getbit bitmap:public:aof:auto $sparse_public_offset]

        if {$test_high_offset} {
            r config set proto-max-bulk-len $byte_len
            assert_equal 1 [r getbit bitmap:public:aof:high $high_bit]
            assert_equal 1 [r bitcount bitmap:public:aof:high]
            r config set proto-max-bulk-len $old_limit
        }
        r config set bitmap-default-roaring no
    }

    test {AOF rewrite preserves Roaring and string bitmap objects} {
        r flushall
        r config set appendonly yes
        waitForBgrewriteaof r
        r config set auto-aof-rewrite-percentage 0

        set raw [binary format H* 80400100080000]
        r set bitmap:aof:transition:roaring $raw
        convert_string_bitmap_to_roaring r bitmap:aof:transition:roaring
        r set bitmap:aof:transition:string $raw

        assert_equal bitmap [r type bitmap:aof:transition:roaring]
        assert_equal string [r type bitmap:aof:transition:string]
        set digest_before [debug_digest]

        r bgrewriteaof
        waitForBgrewriteaof r
        r debug loadaof

        assert_equal [debug_digest] $digest_before
        assert_equal bitmap [r type bitmap:aof:transition:roaring]
        assert_equal bitmap-roaring [r object encoding bitmap:aof:transition:roaring]
        assert_equal $raw [r debug bitmap-raw bitmap:aof:transition:roaring]
        assert_equal string [r type bitmap:aof:transition:string]
        assert_equal $raw [r get bitmap:aof:transition:string]
    }
}

start_server {tags {"bitmap" "bitmap-roaring" "needs:debug" "external:skip" "cluster:skip" "logreqres:skip"} overrides {appendonly yes appendfsync always save {} aof-use-rdb-preamble no}} {
    test {Bitmap transitions use deterministic transactional commands in the incremental AOF} {
        set aof [get_last_incr_aof_path r]
        set raw [binary format H* 80400100080000]

        # SETBIT against a missing key propagates conversion before the write.
        r config set bitmap-default-roaring yes
        r setbit bitmap:aof-incr:create $sparse_public_offset 1
        r config set bitmap-default-roaring no

        # SETBIT against a string uses the same order and preserves its TTL.
        r set bitmap:aof-incr:convert $raw
        r pexpire bitmap:aof-incr:convert 600000
        set convert_expire [r pexpiretime bitmap:aof-incr:convert]
        r config set bitmap-default-roaring yes
        assert_equal 1 [r setbit bitmap:aof-incr:convert 0 1]
        r config set bitmap-default-roaring no

        # BITFIELD also converts first, even when the logical write is a no-op.
        r set bitmap:aof-incr:bitfield $raw
        r pexpire bitmap:aof-incr:bitfield 600000
        set bitfield_expire [r pexpiretime bitmap:aof-incr:bitfield]
        r config set bitmap-default-roaring yes
        assert_equal {1} [r bitfield bitmap:aof-incr:bitfield SET u1 0 1]
        r config set bitmap-default-roaring no

        # A config-driven BITOP replays the native store directly.
        r set bitmap:aof-incr:bitop:s1 [binary format H* f0]
        r set bitmap:aof-incr:bitop:s2 [binary format H* 0f]
        r config set bitmap-default-roaring yes
        assert_equal 1 [r bitop or bitmap:aof-incr:bitop:out \
            bitmap:aof-incr:bitop:s1 bitmap:aof-incr:bitop:s2]
        r set bitmap:aof-incr:bitop:empty keep
        assert_equal 0 [r bitop or bitmap:aof-incr:bitop:empty \
            bitmap:aof-incr:bitop:missing]
        r config set bitmap-default-roaring no

        set fp [open $aof r]
        fconfigure $fp -translation binary
        fconfigure $fp -blocking 1

        set transitions {}
        set transition_restores 0
        while {1} {
            set cmd [read_from_aof $fp]
            if {$cmd eq ""} break
            set name [lindex $cmd 0]
            if {$name in {multi exec bitconvert setbit bitfield bitop bitroarop}} {
                lappend transitions $cmd
            }
            if {$name eq "restore" && [string match "bitmap:aof-incr:*" [lindex $cmd 1]]} {
                incr transition_restores
            }
        }
        close $fp

        assert_equal [list \
            {multi} \
            [list bitconvert bitmap:aof-incr:create] \
            [list setbit bitmap:aof-incr:create $sparse_public_offset 1] \
            {exec} \
            {multi} \
            [list bitconvert bitmap:aof-incr:convert] \
            [list setbit bitmap:aof-incr:convert 0 1] \
            {exec} \
            {multi} \
            [list bitconvert bitmap:aof-incr:bitfield] \
            [list bitfield bitmap:aof-incr:bitfield SET u1 0 1] \
            {exec} \
            [list bitroarop or bitmap:aof-incr:bitop:out \
                bitmap:aof-incr:bitop:s1 bitmap:aof-incr:bitop:s2] \
            [list bitop or bitmap:aof-incr:bitop:empty \
                bitmap:aof-incr:bitop:missing]] $transitions
        assert_equal 0 $transition_restores

        set digest_before [debug_digest]
        r debug loadaof
        assert_equal $digest_before [debug_digest]
        assert_equal bitmap [r type bitmap:aof-incr:create]
        assert_equal bitmap [r type bitmap:aof-incr:convert]
        assert_equal $raw [r debug bitmap-raw bitmap:aof-incr:convert]
        assert_equal $convert_expire [r pexpiretime bitmap:aof-incr:convert]
        assert_equal bitmap [r type bitmap:aof-incr:bitfield]
        assert_equal $raw [r debug bitmap-raw bitmap:aof-incr:bitfield]
        assert_equal $bitfield_expire [r pexpiretime bitmap:aof-incr:bitfield]
        assert_equal bitmap [r type bitmap:aof-incr:bitop:out]
        assert_equal [binary format H* ff] [r debug bitmap-raw bitmap:aof-incr:bitop:out]
        assert_equal 0 [r exists bitmap:aof-incr:bitop:empty]
    }

    test {Native BITOP into a logically expired destination survives AOF replay} {
        r debug set-active-expire 0
        r config set bitmap-default-roaring yes
        r setbit bitmap:aof-expired:roaring 100 1
        r set bitmap:aof-expired:string [binary format H* f0]
        r set bitmap:aof-expired:dest1 x PX 1
        r set bitmap:aof-expired:dest2 x PX 1
        after 10

        # A Roaring source, and a string source with a config-selected result.
        # BITOP expires the old destination, whose DEL must be replayed before
        # BITOP rather than after it.
        assert_equal 13 [r bitop or bitmap:aof-expired:dest1 bitmap:aof-expired:roaring]
        assert_equal 1 [r bitop or bitmap:aof-expired:dest2 bitmap:aof-expired:string]
        r config set bitmap-default-roaring no

        set digest_before [debug_digest]
        r debug loadaof
        assert_equal $digest_before [debug_digest]
        assert_equal bitmap [r type bitmap:aof-expired:dest1]
        assert_equal bitmap [r type bitmap:aof-expired:dest2]
        r debug set-active-expire 1
    } {OK}

    test {AOF replay cannot create Roaring bitmaps the RDB loader rejects} {
        # AOF clients skip the proto-max-bulk-len offset check. 32-bit builds
        # must still keep byte lengths within SIZE_MAX, which their RDB loader
        # requires: byte 2^32-2 (length SIZE_MAX) is the last one a write may
        # reach there, and byte 2^32-1 is rejected. 64-bit builds accept both.
        set last_ok_bit [expr {(1 << 35) - 9}]   ;# last bit of byte 2^32-2
        set last_ok_byte [expr {(1 << 35) - 16}] ;# first bit of byte 2^32-2
        set first_bad_bit [expr {(1 << 35) - 8}] ;# first bit of byte 2^32-1
        set keys {}
        foreach key {setbit:ok setbit:bad bitfield:ok bitfield:bad} {
            lappend keys bitmap:aof-wide:$key
        }
        set aof [get_last_incr_aof_path r]
        set fp [open $aof a]
        fconfigure $fp -translation binary
        # the client uses db 9 by default, db 0 under --singledb
        puts -nonewline $fp [formatCommand select [expr {$::singledb ? 0 : 9}]]
        foreach key $keys {
            puts -nonewline $fp [formatCommand bitconvert $key]
        }
        puts -nonewline $fp [formatCommand setbit bitmap:aof-wide:setbit:ok $last_ok_bit 1]
        puts -nonewline $fp [formatCommand setbit bitmap:aof-wide:setbit:bad $first_bad_bit 1]
        puts -nonewline $fp [formatCommand bitfield bitmap:aof-wide:bitfield:ok \
            SET u8 $last_ok_byte 255]
        # An out-of-range field rejects the whole command, including the
        # in-range field before it.
        puts -nonewline $fp [formatCommand bitfield bitmap:aof-wide:bitfield:bad \
            SET u8 0 255 SET u8 $first_bad_bit 255]
        close $fp

        # 32-bit replay rejects the bad writes: log those errors, don't panic.
        set old_behavior [config_get_set propagation-error-behavior ignore]
        r debug loadaof
        r config set propagation-error-behavior $old_behavior

        set wide [expr {[s arch_bits] == 64}]
        foreach reload {0 1} {
            if {$reload} {r debug reload}
            foreach key $keys {
                assert_equal bitmap [r type $key]
            }
            assert_equal 1 [r bitcount bitmap:aof-wide:setbit:ok]
            assert_equal $last_ok_bit [r bitpos bitmap:aof-wide:setbit:ok 1]
            assert_equal 8 [r bitcount bitmap:aof-wide:bitfield:ok]
            assert_equal [expr {$wide ? 1 : 0}] [r bitcount bitmap:aof-wide:setbit:bad]
            assert_equal [expr {$wide ? 16 : 0}] [r bitcount bitmap:aof-wide:bitfield:bad]
        }
        # BITOP reports the longest source length, SIZE_MAX on 32-bit.
        assert_equal [expr {(1 << 32) - 1}] [r bitop or bitmap:aof-wide:out \
            bitmap:aof-wide:setbit:ok bitmap:aof-wide:bitfield:ok]
        r del {*}$keys bitmap:aof-wide:out
    }
}

tags {"bitmap" "bitmap-roaring" "aof" "external:skip" "cluster:skip" "logreqres:skip"} {
    # AOFs written before scripts were replicated by effects hold the EVAL
    # itself. Its writes run on the script client, but replaying it obeys the
    # AOF all the same and must not apply the local configuration.
    set server_path [tmpdir server.bitmap-aof-eval]
    set aof_dirpath "$server_path/appendonlydir"
    create_aof $aof_dirpath "$aof_dirpath/appendonly.aof.1$::incr_aof_suffix$::aof_format_suffix" {
        append_to_aof [formatCommand select 0]
        append_to_aof [formatCommand eval {return redis.call('setbit', KEYS[1], 1, 1)} 1 bitmap:aof-eval]
        # The complement of an empty 1 GiB value, which a writer with a 1 GiB
        # proto-max-bulk-len accepted. pcall keeps a rejection silent.
        append_to_aof [formatCommand bitconvert bitop:aof-eval-not:src]
        append_to_aof [formatCommand setbit bitop:aof-eval-not:src [expr {1024 * 1024 * 1024 * 8 - 1}] 0]
        append_to_aof [formatCommand eval {redis.pcall('bitop', 'not', KEYS[1], KEYS[2]) return 1} 2 bitop:aof-eval-not:dest bitop:aof-eval-not:src]
    }
    create_aof_manifest $aof_dirpath "$aof_dirpath/appendonly.aof$::manifest_suffix" {
        append_to_manifest "file appendonly.aof.1$::incr_aof_suffix$::aof_format_suffix seq 1 type i\n"
    }

    start_server [list overrides [list dir $server_path appendonly yes bitmap-default-roaring yes] keep_persistence true] {
        test {bitmap-default-roaring yes: scripts replayed from the AOF keep strings} {
            r select 0
            assert_equal string [r type bitmap:aof-eval]
            assert_equal [binary format H* 40] [r get bitmap:aof-eval]
        }

        test {scripts replayed from the AOF obey the writer's BITOP NOT missing-chunk budget} {
            r select 0
            assert_equal bitmap [r type bitop:aof-eval-not:dest]
            assert_equal [expr {1024 * 1024 * 1024 * 8}] [r bitcount bitop:aof-eval-not:dest]
        }
    }
}

start_server {tags {"bitmap" "bitmap-roaring" "repl" "external:skip" "cluster:skip"}} {
    start_server {} {
        set master [srv -1 client]
        set master_host [srv -1 host]
        set master_port [srv -1 port]
        set replica [srv 0 client]

        $replica replicaof $master_host $master_port
        wait_for_sync $replica
        wait_for_ofs_sync $master $replica

        test {Roaring bitmap public creation replicates deterministic type transitions} {
            # The replica stays in bitmap-default-roaring no: type decisions must arrive
            # from the master as explicit BITCONVERT commands, never be re-derived from
            # replica-local configuration.
            $master config set bitmap-default-roaring yes
            $replica config set bitmap-default-roaring no

            $master setbit bitmap:public:repl:direct $sparse_public_offset 1
            $master setbit bitmap:public:repl:zero 0 0
            $master set bitmap:public:repl:auto ""
            $master setbit bitmap:public:repl:auto $sparse_public_offset 1
            $master set bitmap:public:repl:bitfield [binary format H* 80]
            assert_equal {1} [$master bitfield bitmap:public:repl:bitfield SET u1 0 1]
            wait_for_ofs_sync $master $replica

            assert_equal bitmap [$replica type bitmap:public:repl:direct]
            assert_equal bitmap [$replica type bitmap:public:repl:zero]
            assert_equal bitmap [$replica type bitmap:public:repl:auto]
            assert_equal bitmap [$replica type bitmap:public:repl:bitfield]
            assert_equal 1 [$replica getbit bitmap:public:repl:direct $sparse_public_offset]
            assert_equal [binary format H* 00] [$replica debug bitmap-raw bitmap:public:repl:zero]
            assert_equal 1 [$replica getbit bitmap:public:repl:auto $sparse_public_offset]
            assert_equal [binary format H* 80] \
                [$replica debug bitmap-raw bitmap:public:repl:bitfield]
            assert_error {WRONGTYPE*} {$replica get bitmap:public:repl:direct}
            assert_error {WRONGTYPE*} {$replica get bitmap:public:repl:auto}
            assert_equal [$master debug digest] [$replica debug digest]
        }

        test {plain SETBIT on an existing Roaring bitmap replicates as a command} {
            # After the explicit BITCONVERT transition, later writes replicate
            # as plain SETBITs against the same type on both sides.
            $master setbit bitmap:public:repl:direct 12345 1
            wait_for_ofs_sync $master $replica

            assert_equal bitmap [$replica type bitmap:public:repl:direct]
            assert_equal 1 [$replica getbit bitmap:public:repl:direct 12345]
        }

        if {[s 0 arch_bits] == 64} {
            test {replicated sparse high offsets remain compact on a lower-limit replica} {
                set high_bit [expr {(1 << 40) - 1}]
                set byte_len [expr {($high_bit >> 3) + 1}]
                set master_limit [lindex [$master config get proto-max-bulk-len] 1]
                set replica_limit [lindex [$replica config get proto-max-bulk-len] 1]

                $master config set proto-max-bulk-len $byte_len
                $replica config set proto-max-bulk-len 536870912
                $master config set bitmap-default-roaring yes
                $master setbit bitmap:repl:high 0 1
                wait_for_ofs_sync $master $replica

                # This write is propagated as SETBIT. Replication obeys the
                # master's accepted command even though the replica's local
                # protocol limit is lower.
                $master setbit bitmap:repl:high $high_bit 1
                wait_for_ofs_sync $master $replica
                assert_equal bitmap [$replica type bitmap:repl:high]
                assert_lessthan [$replica memory usage bitmap:repl:high] 65536

                # DUMP exercises persistence on the lower-limit replica without
                # materializing its 128 GiB logical string length.
                set payload [$replica dump bitmap:repl:high]
                assert_lessthan [string length $payload] 256

                $replica config set proto-max-bulk-len $byte_len
                assert_equal 1 [$replica getbit bitmap:repl:high $high_bit]
                assert_equal 2 [$replica bitcount bitmap:repl:high]
                $master config set proto-max-bulk-len $master_limit
                $replica config set proto-max-bulk-len $replica_limit
            }

            test {replicated BITOP NOT obeys the master's missing-chunk budget} {
                # The BITOP NOT budget follows proto-max-bulk-len. A replica
                # with a lower limit must still apply the master's accepted
                # complement of an empty 1 GiB value.
                set byte_len [expr {1024 * 1024 * 1024}]
                set master_limit [lindex [$master config get proto-max-bulk-len] 1]
                set replica_limit [lindex [$replica config get proto-max-bulk-len] 1]

                $master config set proto-max-bulk-len $byte_len
                $replica config set proto-max-bulk-len 536870912
                $master config set bitmap-default-roaring yes
                $master del bitop:repl:not:src bitop:repl:not:dest
                $master setbit bitop:repl:not:src [expr {$byte_len * 8 - 1}] 0
                assert_equal $byte_len \
                    [$master bitop not bitop:repl:not:dest bitop:repl:not:src]
                wait_for_ofs_sync $master $replica

                assert_equal bitmap [$replica type bitop:repl:not:dest]
                assert_equal [expr {$byte_len * 8}] \
                    [$replica bitcount bitop:repl:not:dest]
                assert_equal [$master debug digest] [$replica debug digest]

                $master del bitop:repl:not:src bitop:repl:not:dest
                wait_for_ofs_sync $master $replica
                $master config set proto-max-bulk-len $master_limit
                $replica config set proto-max-bulk-len $replica_limit
            }
        }

        test {BITOP destinations replicate deterministically across modes} {
            # String-only sources with a bitmap-default-roaring yes master: the
            # destination decision is master-local, so the stream carries the
            # internal BITROAROP command.
            $master del bitop:repl:s1 bitop:repl:s2 bitop:repl:out
            $master set bitop:repl:s1 [binary format H* f0]
            $master set bitop:repl:s2 [binary format H* 0f]
            $master config set bitmap-default-roaring yes
            $master bitop or bitop:repl:out bitop:repl:s1 bitop:repl:s2
            wait_for_ofs_sync $master $replica
            assert_equal bitmap [$replica type bitop:repl:out]
            assert_equal [$master debug digest] [$replica debug digest]

            # The reverse mismatch: a bitmap-default-roaring no master with a
            # bitmap-default-roaring yes replica. The replica obeys the replicated BITOP
            # verbatim and must not natify its destination.
            $master config set bitmap-default-roaring no
            $replica config set bitmap-default-roaring yes
            $master bitop and bitop:repl:out2 bitop:repl:s1 bitop:repl:s2
            wait_for_ofs_sync $master $replica
            assert_equal string [$master type bitop:repl:out2]
            assert_equal string [$replica type bitop:repl:out2]
            assert_equal [$master debug digest] [$replica debug digest]
            $replica config set bitmap-default-roaring no
        }

        test {Roaring bitmaps survive a full resync as bitmaps} {
            # Detach and wipe the replica, then reattach: the keys now arrive
            # through the RDB-over-the-wire full sync path instead of the
            # command stream.
            $replica replicaof no one
            $replica flushall
            $replica replicaof $master_host $master_port
            wait_for_condition 50 100 {
                [s 0 master_link_status] eq {up}
            } else {
                fail "Replication not restarted."
            }
            wait_for_ofs_sync $master $replica

            assert_equal bitmap [$replica type bitmap:public:repl:direct]
            assert_equal bitmap [$replica type bitmap:public:repl:auto]
            assert_equal bitmap [$replica type bitmap:public:repl:bitfield]
            assert_equal 1 [$replica getbit bitmap:public:repl:direct $sparse_public_offset]
            assert_equal 1 [$replica getbit bitmap:public:repl:direct 12345]
            assert_equal [binary format H* 00] [$replica debug bitmap-raw bitmap:public:repl:zero]
            assert_equal 1 [$replica getbit bitmap:public:repl:auto $sparse_public_offset]
            assert_equal [$master debug digest] [$replica debug digest]
        }

        test {bitmap-default-roaring conversion replicates as BITCONVERT plus SETBIT} {
            set raw [binary format H* 80400100080000]

            $master config set bitmap-default-roaring no
            $master set bitmap:public:repl:conv $raw
            wait_for_ofs_sync $master $replica
            assert_equal string [$replica type bitmap:public:repl:conv]

            # The replica first receives the explicit representation decision,
            # then replays SETBIT against the resulting native bitmap. Its own
            # bitmap-default-roaring setting is irrelevant.
            $master config set bitmap-default-roaring yes
            assert_equal 1 [$master setbit bitmap:public:repl:conv 0 1]
            $master config set bitmap-default-roaring no
            wait_for_ofs_sync $master $replica

            assert_equal bitmap [$replica type bitmap:public:repl:conv]
            assert_equal bitmap-roaring [$replica object encoding bitmap:public:repl:conv]
            assert_equal $raw [$replica debug bitmap-raw bitmap:public:repl:conv]
            assert_equal [$master debug digest] [$replica debug digest]
        }

        test {RESTORE payloads replicate bitmap and string type transitions} {
            set raw [binary format H* 80400100080000]

            $master del bitmap:public:repl:restore:source bitmap:public:repl:restore:target
            $master set bitmap:public:repl:restore:source $raw
            set string_payload [$master dump bitmap:public:repl:restore:source]
            convert_string_bitmap_to_roaring $master bitmap:public:repl:restore:source
            set bitmap_payload [$master dump bitmap:public:repl:restore:source]

            $master restore bitmap:public:repl:restore:target 0 $string_payload replace
            wait_for_ofs_sync $master $replica
            assert_equal string [$replica type bitmap:public:repl:restore:target]
            assert_equal $raw [$replica get bitmap:public:repl:restore:target]

            $master restore bitmap:public:repl:restore:target 0 $bitmap_payload replace
            wait_for_ofs_sync $master $replica
            assert_equal bitmap [$replica type bitmap:public:repl:restore:target]
            assert_equal bitmap-roaring [$replica object encoding bitmap:public:repl:restore:target]
            assert_equal $raw [$replica debug bitmap-raw bitmap:public:repl:restore:target]

            $master restore bitmap:public:repl:restore:target 0 $string_payload replace
            wait_for_ofs_sync $master $replica
            assert_equal string [$replica type bitmap:public:repl:restore:target]
            assert_equal $raw [$replica get bitmap:public:repl:restore:target]
            assert_equal [$master debug digest] [$replica debug digest]
        }

        test {writable replica ignores bitmap-default-roaring for local writes} {
            # The master owns the representation decision. Local writes on a
            # writable replica must not convert a key the master owns, not even
            # logical no-ops, or the master's later string writes to it would
            # fail on the replica with WRONGTYPE.
            $master config set bitmap-default-roaring no
            $replica config set bitmap-default-roaring yes
            $replica config set replica-read-only no

            $master set bitmap:repl:writable abc
            wait_for_ofs_sync $master $replica
            assert_equal abc [$replica get bitmap:repl:writable]

            assert_equal 0 [$replica setbit bitmap:repl:writable 0 0]
            assert_equal {0} [$replica bitfield bitmap:repl:writable SET u1 0 0]
            assert_equal string [$replica type bitmap:repl:writable]

            assert_equal 6 [$master append bitmap:repl:writable def]
            wait_for_ofs_sync $master $replica
            assert_equal string [$replica type bitmap:repl:writable]
            assert_equal abcdef [$replica get bitmap:repl:writable]

            # Keys created by local writes stay plain strings as well.
            set local_keys {bitmap:repl:writable:setbit bitmap:repl:writable:bitfield
                            bitmap:repl:writable:bitop}
            assert_equal 0 [$replica setbit bitmap:repl:writable:setbit 7 1]
            assert_equal {0} [$replica bitfield bitmap:repl:writable:bitfield SET u8 0 255]
            assert_equal 6 [$replica bitop or bitmap:repl:writable:bitop bitmap:repl:writable]
            foreach key $local_keys {
                assert_equal string [$replica type $key]
            }

            $replica del {*}$local_keys
            $replica config set replica-read-only yes
            $replica config set bitmap-default-roaring no
            assert_equal [$master debug digest] [$replica debug digest]
        }
    }
}

start_server {tags {"bitmap" "bitmap-roaring" "repl" "aof" "needs:debug" "external:skip" "cluster:skip" "logreqres:skip"} overrides {appendonly yes appendfsync always save {} aof-use-rdb-preamble no auto-aof-rewrite-percentage 0}} {
    start_server {} {
        set master [srv -1 client]
        set master_host [srv -1 host]
        set master_port [srv -1 port]
        set replica [srv 0 client]

        $replica replicaof $master_host $master_port
        wait_for_sync $replica

        test {Bitmap transition primitives honor Lua selective propagation targets} {
            $master flushall
            wait_for_ofs_sync $master $replica
            $master set bitmap:selective:source [binary format H* f0]
            wait_for_ofs_sync $master $replica
            $master bgrewriteaof
            waitForBgrewriteaof $master

            $master config set bitmap-default-roaring yes
            $replica config set bitmap-default-roaring no
            set cases {}

            # The configuration only applies when the transition reaches both
            # the AOF and the replicas: a target missing it would apply later
            # writes to a string. Restricted scripts keep strings everywhere.
            foreach {mode constant on_replica in_aof type} {
                none REPL_NONE 0 0 string
                aof REPL_AOF 0 1 string
                replica REPL_REPLICA 1 0 string
                all REPL_ALL 1 1 bitmap
            } {
                foreach command {setbit bitfield bitop} {
                    set key bitmap:selective:$mode:$command
                    set aof_size_before [status $master aof_current_size]
                    if {$command eq "setbit"} {
                        set script [format {
                            redis.set_repl(redis.%s)
                            return redis.call('SETBIT', KEYS[1], 0, 1)
                        } $constant]
                        assert_equal 0 [$master eval $script 1 $key]
                    } elseif {$command eq "bitfield"} {
                        set script [format {
                            redis.set_repl(redis.%s)
                            return redis.call('BITFIELD', KEYS[1], 'SET', 'u8', 0, 255)
                        } $constant]
                        assert_equal {0} [$master eval $script 1 $key]
                    } else {
                        set script [format {
                            redis.set_repl(redis.%s)
                            return redis.call('BITOP', 'OR', KEYS[1], KEYS[2])
                        } $constant]
                        assert_equal 1 [$master eval $script 2 $key bitmap:selective:source]
                    }
                    if {!$in_aof} {
                        assert_equal $aof_size_before [status $master aof_current_size]
                    }
                    assert_equal $type [$master type $key]
                    lappend cases $key $on_replica $in_aof $type
                }
            }

            wait_for_ofs_sync $master $replica
            foreach {key on_replica in_aof type} $cases {
                if {$on_replica} {
                    assert_equal $type [$replica type $key]
                } else {
                    assert_equal none [$replica type $key]
                }
            }

            # Detach before replacing the master's live dataset from its AOF;
            # only REPL_AOF and REPL_ALL writes should be present.
            $replica replicaof no one
            $master debug loadaof
            foreach {key on_replica in_aof type} $cases {
                if {$in_aof} {
                    assert_equal $type [$master type $key]
                } else {
                    assert_equal none [$master type $key]
                }
            }
        }
    }
}

# Note: the notify-race tests that mutated the key from a module keyspace
# notification callback ("new", "overwritten" and "type_changed" variants,
# via tests/modules/bitmap_notify.c) are removed. With dbAddInternal() and
# setKeyByLink() restored to the upstream shape, such a mutation hits the
# pre-existing upstream use-after-free (the post-notification bookkeeping
# dereferences the possibly freed value). Re-add them once the upstream fix
# lands.

proc seed_string_bitmap {key bits} {
    r del $key
    r set $key ""
    foreach bit $bits {
        r setbit $key $bit 1
    }
}

# Logical raw bytes of a bitmap value regardless of its representation.
proc bitmap_logical_raw {key} {
    if {![r exists $key]} {
        return ""
    }
    if {[r type $key] eq "bitmap"} {
        return [r debug bitmap-raw $key]
    }
    return [r get $key]
}

proc assert_bitmap_has_exact_bits {key bits} {
    set unique [lsort -integer -unique $bits]
    assert_equal [llength $unique] [r bitcount $key]
    foreach bit $unique {
        assert_equal 1 [r getbit $key $bit]
    }
}

proc assert_bitmap_translated_jaccard {name left_bits right_bits expected_intersection expected_union expected_ratio} {
    set left "bitmap:roaring:translated:jaccard:$name:left"
    set right "bitmap:roaring:translated:jaccard:$name:right"
    set intersection "bitmap:roaring:translated:jaccard:$name:intersection"
    set union "bitmap:roaring:translated:jaccard:$name:union"

    seed_roaring_bitmap $left $left_bits
    seed_roaring_bitmap $right $right_bits

    r bitop and $intersection $left $right
    r bitop or $union $left $right

    set actual_intersection [r bitcount $intersection]
    set actual_union [r bitcount $union]
    assert_equal $expected_intersection $actual_intersection
    assert_equal $expected_union $actual_union
    if {$actual_union == 0} {
        set actual_ratio -1
    } else {
        set actual_ratio [format %.6f [expr {double($actual_intersection) / $actual_union}]]
    }
    assert_equal $expected_ratio $actual_ratio
}

proc assert_roaring_bitop_matches_string {name op source_bitsets} {
    set string_dest "bitmap:roaring:bitop:$name:string:dest"
    set roaring_dest "bitmap:roaring:bitop:$name:roaring:dest"
    set string_sources {}
    set roaring_sources {}

    for {set i 0} {$i < [llength $source_bitsets]} {incr i} {
        set string_key "bitmap:roaring:bitop:$name:string:src:$i"
        set roaring_key "bitmap:roaring:bitop:$name:roaring:src:$i"
        seed_string_bitmap $string_key [lindex $source_bitsets $i]
        seed_roaring_bitmap $roaring_key [lindex $source_bitsets $i]
        lappend string_sources $string_key
        lappend roaring_sources $roaring_key
    }

    set string_reply [r bitop $op $string_dest {*}$string_sources]
    set roaring_reply [r bitop $op $roaring_dest {*}$roaring_sources]
    assert_equal $string_reply $roaring_reply
    assert_equal [bitmap_logical_raw $string_dest] [bitmap_logical_raw $roaring_dest]
    assert_equal $string_reply [string length [bitmap_logical_raw $string_dest]]
    assert_equal $roaring_reply [string length [bitmap_logical_raw $roaring_dest]]
    if {[r exists $roaring_dest]} {
        # At least one roaring source makes the destination roaring.
        assert_equal bitmap [r type $roaring_dest]
        assert_equal string [r type $string_dest]
    }
}

proc assert_roaring_bitop_bitset_case {name op source_bitsets expected_bits {missing_indexes {}} {alias_index -1} {dest_seed __none__}} {
    set string_dest "bitmap:roaring:bitop:case:$name:string:dest"
    set roaring_dest "bitmap:roaring:bitop:case:$name:roaring:dest"
    set string_sources {}
    set roaring_sources {}
    set string_source_raws {}
    set roaring_source_raws {}

    r config set bitmap-default-roaring no

    if {$dest_seed eq "__none__"} {
        r del $string_dest $roaring_dest
    } else {
        seed_string_bitmap $string_dest $dest_seed
        seed_roaring_bitmap $roaring_dest $dest_seed
    }

    for {set i 0} {$i < [llength $source_bitsets]} {incr i} {
        set string_key "bitmap:roaring:bitop:case:$name:string:src:$i"
        set roaring_key "bitmap:roaring:bitop:case:$name:roaring:src:$i"
        if {[lsearch -exact $missing_indexes $i] >= 0} {
            r del $string_key $roaring_key
        } else {
            seed_string_bitmap $string_key [lindex $source_bitsets $i]
            seed_roaring_bitmap $roaring_key [lindex $source_bitsets $i]
        }
        lappend string_sources $string_key
        lappend roaring_sources $roaring_key
        lappend string_source_raws [bitmap_logical_raw $string_key]
        lappend roaring_source_raws [bitmap_logical_raw $roaring_key]
    }

    if {$alias_index >= 0} {
        set string_dest [lindex $string_sources $alias_index]
        set roaring_dest [lindex $roaring_sources $alias_index]
    }

    set string_reply [r bitop $op $string_dest {*}$string_sources]
    set roaring_reply [r bitop $op $roaring_dest {*}$roaring_sources]
    assert_equal $string_reply $roaring_reply
    assert_equal [bitmap_logical_raw $string_dest] [bitmap_logical_raw $roaring_dest]
    assert_equal $string_reply [string length [bitmap_logical_raw $string_dest]]
    assert_equal $roaring_reply [string length [bitmap_logical_raw $roaring_dest]]
    assert_bitmap_has_exact_bits $string_dest $expected_bits
    assert_bitmap_has_exact_bits $roaring_dest $expected_bits
    if {[r exists $roaring_dest]} {
        assert_equal bitmap [r type $roaring_dest]
        assert_equal bitmap-roaring [r object encoding $roaring_dest]
    }

    for {set i 0} {$i < [llength $source_bitsets]} {incr i} {
        if {$i == $alias_index} continue
        assert_equal [lindex $string_source_raws $i] [bitmap_logical_raw [lindex $string_sources $i]]
        assert_equal [lindex $roaring_source_raws $i] [bitmap_logical_raw [lindex $roaring_sources $i]]
    }
}

proc assert_roaring_bitop_raws_match_string {name op source_raws roaring_indexes {alias_index -1}} {
    set string_dest "bitmap:roaring:bitop:$name:string:dest"
    set roaring_dest "bitmap:roaring:bitop:$name:roaring:dest"
    set string_sources {}
    set roaring_sources {}
    set string_source_raws {}
    set roaring_source_raws {}

    r config set bitmap-default-roaring no

    for {set i 0} {$i < [llength $source_raws]} {incr i} {
        set string_key "bitmap:roaring:bitop:$name:string:src:$i"
        set roaring_key "bitmap:roaring:bitop:$name:roaring:src:$i"
        r set $string_key [lindex $source_raws $i]
        r set $roaring_key [lindex $source_raws $i]
        if {[lsearch -exact $roaring_indexes $i] >= 0} {
            convert_string_bitmap_to_roaring r $roaring_key
        }
        lappend string_sources $string_key
        lappend roaring_sources $roaring_key
        lappend string_source_raws [bitmap_logical_raw $string_key]
        lappend roaring_source_raws [bitmap_logical_raw $roaring_key]
    }

    if {$alias_index >= 0} {
        set string_dest [lindex $string_sources $alias_index]
        set roaring_dest [lindex $roaring_sources $alias_index]
    }

    set string_reply [r bitop $op $string_dest {*}$string_sources]
    set roaring_reply [r bitop $op $roaring_dest {*}$roaring_sources]
    assert_equal $string_reply $roaring_reply
    assert_equal [bitmap_logical_raw $string_dest] [bitmap_logical_raw $roaring_dest]
    assert_equal $string_reply [string length [bitmap_logical_raw $string_dest]]
    assert_equal $roaring_reply [string length [bitmap_logical_raw $roaring_dest]]
    if {[r exists $roaring_dest] && [llength $roaring_indexes] > 0} {
        assert_equal bitmap [r type $roaring_dest]
        assert_equal string [r type $string_dest]
    }

    for {set i 0} {$i < [llength $source_raws]} {incr i} {
        if {$i == $alias_index} continue
        assert_equal [lindex $string_source_raws $i] [bitmap_logical_raw [lindex $string_sources $i]]
        assert_equal [lindex $roaring_source_raws $i] [bitmap_logical_raw [lindex $roaring_sources $i]]
    }
}

proc assert_roaring_bitmap_command_matches_string {name raw command} {
    set string_key "bitmap:roaring:read-edge:$name:string"
    set roaring_key "bitmap:roaring:read-edge:$name:roaring"
    r set $string_key $raw
    r set $roaring_key $raw
    convert_string_bitmap_to_roaring r $roaring_key
    set string_cmd [lreplace $command 1 1 $string_key]
    set roaring_cmd [lreplace $command 1 1 $roaring_key]
    assert_equal [r {*}$string_cmd] [r {*}$roaring_cmd]
    assert_equal bitmap [r type $roaring_key]
    assert_equal bitmap-roaring [r object encoding $roaring_key]
}

proc assert_roaring_bitmap_write_matches_string {name raw command} {
    set string_key "bitmap:roaring:write-edge:$name:string"
    set roaring_key "bitmap:roaring:write-edge:$name:roaring"
    r set $string_key $raw
    r set $roaring_key $raw
    convert_string_bitmap_to_roaring r $roaring_key
    set string_cmd [lreplace $command 1 1 $string_key]
    set roaring_cmd [lreplace $command 1 1 $roaring_key]
    assert_equal [r {*}$string_cmd] [r {*}$roaring_cmd]
    assert_equal [bitmap_logical_raw $string_key] [r debug bitmap-raw $roaring_key]
    assert_equal bitmap [r type $roaring_key]
    assert_equal bitmap-roaring [r object encoding $roaring_key]
}

start_server {tags {"bitmap" "bitmap-roaring" "needs:debug" "cluster:skip"}} {
    test {Roaring bitmap read commands preserve type encoding and bytes} {
        set raw [binary format H* 80400100080000]

        r set bitmap:roaring:read $raw
        convert_string_bitmap_to_roaring r bitmap:roaring:read
        assert_equal 1 [r getbit bitmap:roaring:read 0]
        assert_equal 1 [r getbit bitmap:roaring:read 9]
        assert_equal 0 [r getbit bitmap:roaring:read 10]
        assert_equal 4 [r bitcount bitmap:roaring:read]
        assert_equal 2 [r bitcount bitmap:roaring:read 8 23 bit]
        assert_equal 0 [r bitpos bitmap:roaring:read 1]
        assert_equal 1 [r bitpos bitmap:roaring:read 0]
        assert_equal 9 [r bitpos bitmap:roaring:read 1 8 -1 bit]
        assert_equal {1 1 1} [r bitfield_ro bitmap:roaring:read GET u1 0 GET u1 9 GET u1 36]
        assert_error {ERR BITFIELD_RO only supports the GET subcommand} {
            r bitfield_ro bitmap:roaring:read SET u8 0 255
        }

        assert_equal bitmap [r type bitmap:roaring:read]
        assert_equal bitmap-roaring [r object encoding bitmap:roaring:read]
        assert_equal $raw [r debug bitmap-raw bitmap:roaring:read]
    }

    test {bitmap-default-roaring conversion preserves dense raw chunks and boundary bits} {
        set raw [binary format H* "[string repeat ff 8192]8001"]

        r set bitmap:roaring:convert:dense $raw
        r config set bitmap-default-roaring yes
        assert_equal 1 [r setbit bitmap:roaring:convert:dense 0 1]
        r config set bitmap-default-roaring no
        assert_equal bitmap [r type bitmap:roaring:convert:dense]
        assert_equal bitmap-roaring [r object encoding bitmap:roaring:convert:dense]
        assert_equal $raw [r debug bitmap-raw bitmap:roaring:convert:dense]
        assert_equal 65538 [r bitcount bitmap:roaring:convert:dense]
        assert_equal 1 [r getbit bitmap:roaring:convert:dense 0]
        assert_equal 1 [r getbit bitmap:roaring:convert:dense 65535]
        assert_equal 1 [r getbit bitmap:roaring:convert:dense 65536]
        assert_equal 1 [r getbit bitmap:roaring:convert:dense 65551]
    }

    test {SETBIT and GETBIT round trip Roaring bitmap offsets} {
        seed_roaring_bitmap bitmap:roaring:setbit:loop {}

        for {set offset 0} {$offset < 100} {incr offset} {
            assert_equal 0 [r setbit bitmap:roaring:setbit:loop $offset 1]
            assert_equal 1 [r getbit bitmap:roaring:setbit:loop $offset]
            assert_equal 1 [r setbit bitmap:roaring:setbit:loop $offset 0]
            assert_equal 0 [r getbit bitmap:roaring:setbit:loop $offset]
        }

        assert_equal bitmap [r type bitmap:roaring:setbit:loop]
        assert_equal bitmap-roaring [r object encoding bitmap:roaring:setbit:loop]
        assert_equal 0 [r bitcount bitmap:roaring:setbit:loop]
    }

    test {SETBIT updates existing Roaring bitmap keys through direct Roaring path} {
        r config set bitmap-default-roaring yes
        r del bitmap:roaring:setbit:existing

        assert_equal 0 [r setbit bitmap:roaring:setbit:existing 5 1]
        assert_equal bitmap [r type bitmap:roaring:setbit:existing]

        r config set bitmap-default-roaring no
        assert_equal 0 [r setbit bitmap:roaring:setbit:existing 6 1]
        assert_equal bitmap [r type bitmap:roaring:setbit:existing]
        assert_equal bitmap-roaring [r object encoding bitmap:roaring:setbit:existing]
        assert_equal 1 [r getbit bitmap:roaring:setbit:existing 6]
        assert_equal 2 [r bitcount bitmap:roaring:setbit:existing]
    }

    test {SETBIT updates Roaring bitmap values and preserves trailing zero length} {
        r set bitmap:roaring:setbit [binary format H* 8000]
        convert_string_bitmap_to_roaring r bitmap:roaring:setbit
        assert_equal 0 [r setbit bitmap:roaring:setbit 9 1]
        assert_equal bitmap [r type bitmap:roaring:setbit]
        assert_equal bitmap-roaring [r object encoding bitmap:roaring:setbit]
        assert_equal [binary format H* 8040] [r debug bitmap-raw bitmap:roaring:setbit]

        assert_equal 0 [r setbit bitmap:roaring:setbit 23 0]
        assert_equal bitmap [r type bitmap:roaring:setbit]
        assert_equal [binary format H* 804000] [r debug bitmap-raw bitmap:roaring:setbit]

        assert_equal 1 [r setbit bitmap:roaring:setbit 0 0]
        assert_equal bitmap [r type bitmap:roaring:setbit]
        assert_equal [binary format H* 004000] [r debug bitmap-raw bitmap:roaring:setbit]
    }

    test {Roaring bitmap MEMORY USAGE tracks roaring container allocation updates} {
        r config set bitmap-default-roaring yes
        r del bitmap:roaring:memory

        assert_equal 0 [r setbit bitmap:roaring:memory 0 1]
        set one_container [r memory usage bitmap:roaring:memory]
        assert_morethan $one_container 0

        assert_equal 0 [r setbit bitmap:roaring:memory 65536 1]
        set two_containers [r memory usage bitmap:roaring:memory]
        assert_morethan $two_containers $one_container

        assert_equal 1 [r setbit bitmap:roaring:memory 65536 0]
        set back_to_one [r memory usage bitmap:roaring:memory]
        assert_lessthan $back_to_one $two_containers
        assert_equal 1 [r bitcount bitmap:roaring:memory]

        assert_equal 1 [r setbit bitmap:roaring:memory 0 0]
        set empty [r memory usage bitmap:roaring:memory]
        assert_lessthan $empty $back_to_one
        assert_equal 0 [r bitcount bitmap:roaring:memory]

        r del bitmap:roaring:memory:same-container
        assert_equal 0 [r setbit bitmap:roaring:memory:same-container 0 1]
        set sparse_container [r memory usage bitmap:roaring:memory:same-container]
        for {set bit 1} {$bit <= 4096} {incr bit} {
            assert_equal 0 [r setbit bitmap:roaring:memory:same-container $bit 1]
        }
        set dense_container [r memory usage bitmap:roaring:memory:same-container]
        assert_morethan $dense_container $sparse_container
        assert_equal 4097 [r bitcount bitmap:roaring:memory:same-container]

        r config set bitmap-default-roaring no
        r del bitmap:roaring:memory bitmap:roaring:memory:same-container
    }

    if {[string match {*jemalloc*} [s mem_allocator]]} {
        test {Roaring BITSET container words stay in jemalloc's 8 KiB size class} {
            # Alternating bits make every chunk a BITSET container with 8 KiB of
            # aligned words. Over-allocating them for the alignment would move
            # each buffer into jemalloc's 10 KiB size class, so a container
            # would cost about 10 KiB instead of a little over 8 KiB.
            set containers 100
            set key bitmap:roaring:memory:bitsets
            r del $key
            r set $key [string repeat [binary format H* aa] \
                [expr {$containers * 8192}]]
            convert_string_bitmap_to_roaring r $key
            assert_equal bitmap-roaring [r object encoding $key]
            assert_equal [expr {$containers * 32768}] [r bitcount $key]
            set usage [r memory usage $key]
            assert_lessthan [expr {$usage / $containers}] 9216
            r del $key
        }
    }

    test {Roaring bitmap whole-object operations keep lazy memory accounting accurate} {
        r config set bitmap-default-roaring yes
        set source bitmap:roaring:lazy:a
        set copy bitmap:roaring:lazy:b
        set restored bitmap:roaring:lazy:c
        set bitop bitmap:roaring:lazy:d
        set bitop_reference bitmap:roaring:lazy:e
        r del $source $copy $restored $bitop $bitop_reference

        foreach offset {0 65536 131072} {
            assert_equal 0 [r setbit $source $offset 1]
        }
        assert_equal 1 [r copy $source $copy]
        r restore $restored 0 [r dump $source]
        r bitop or $bitop $source
        r bitop or $bitop_reference $source

        # Equivalent bitmaps can receive different allocator usable sizes, so
        # use each independently allocated key as its own accounting baseline.
        set tracked_keys [list $source $copy $restored $bitop $bitop_reference]
        set memory_before {}
        foreach key $tracked_keys {
            assert_equal bitmap-roaring [r object encoding $key]
            assert_equal 3 [r bitcount $key]
            set key_before [r memory usage $key]
            assert_morethan $key_before 0 "key=$key before_mutation"
            dict set memory_before $key $key_before
        }

        foreach key $tracked_keys {
            assert_equal 0 [r setbit $key 196608 1]
            assert_equal 4 [r bitcount $key]
        }
        foreach key $tracked_keys {
            set key_after [r memory usage $key]
            assert_morethan $key_after [dict get $memory_before $key] \
                "key=$key after_mutation"
        }

        r config set bitmap-default-roaring no
        r del $source $copy $restored $bitop $bitop_reference
    }

    test {bitmap commands operate on legacy and Roaring representations with default Roaring creation disabled} {
        r config set bitmap-default-roaring no
        set raw [binary format H* 804001]
        set string_key bitmap:roaring:mixed-surface:string
        set roaring_key bitmap:roaring:mixed-surface:roaring

        r set $string_key $raw
        r set $roaring_key $raw
        convert_string_bitmap_to_roaring r $roaring_key
        assert_equal string [r type $string_key]
        assert_equal bitmap [r type $roaring_key]
        assert_equal bitmap-roaring [r object encoding $roaring_key]

        assert_equal [r setbit $string_key 23 1] [r setbit $roaring_key 23 1]
        assert_equal [r getbit $string_key 23] [r getbit $roaring_key 23]
        assert_equal [r bitcount $string_key] [r bitcount $roaring_key]
        assert_equal [r bitcount $string_key 3 20 bit] [r bitcount $roaring_key 3 20 bit]
        assert_equal [r bitpos $string_key 1] [r bitpos $roaring_key 1]
        assert_equal [r bitpos $string_key 0 4 -1 bit] [r bitpos $roaring_key 0 4 -1 bit]

        set bitfield_cmd {GET u8 0 SET u5 9 17 INCRBY i6 16 -3 GET i6 16}
        assert_equal [r bitfield $string_key {*}$bitfield_cmd] [r bitfield $roaring_key {*}$bitfield_cmd]
        assert_equal [r bitfield_ro $string_key GET u8 0 GET u8 16] [r bitfield_ro $roaring_key GET u8 0 GET u8 16]
        assert_equal [r get $string_key] [r debug bitmap-raw $roaring_key]

        assert_roaring_bitop_raws_match_string mixed-surface:bitop or \
            [list [r get $string_key] [binary format H* 0f00ff]] {0}
    }

    test {GETBIT past the Roaring bitmap logical length returns 0} {
        seed_roaring_bitmap bitmap:roaring:getbit:past {3}

        assert_equal 1 [r getbit bitmap:roaring:getbit:past 3]
        assert_equal 0 [r getbit bitmap:roaring:getbit:past 7]
        assert_equal 0 [r getbit bitmap:roaring:getbit:past 100]
        assert_equal 0 [r getbit bitmap:roaring:getbit:past 4294967295]
        assert_error {*bit offset is*out of range*} {
            r getbit bitmap:roaring:getbit:past 4294967296
        }
        assert_error {*bit offset is*out of range*} {
            r getbit bitmap:roaring:getbit:past 9223372036854775808
        }
        assert_equal [binary format H* 10] [r debug bitmap-raw bitmap:roaring:getbit:past]
    }

    test {Roaring SETBIT offsets above UINT32_MAX follow proto-max-bulk-len} {
        set first_wide_bit 4294967296
        set limit [expr {($first_wide_bit / 8) + 1}]
        set oldval [config_get_set proto-max-bulk-len $limit]
        r config set bitmap-default-roaring yes
        r del bitmap:roaring:wide-offset-cap

        assert_equal 0 [r setbit bitmap:roaring:wide-offset-cap 0 1]
        assert_equal 0 [r setbit bitmap:roaring:wide-offset-cap $first_wide_bit 1]
        assert_equal bitmap [r type bitmap:roaring:wide-offset-cap]
        assert_equal bitmap-roaring [r object encoding bitmap:roaring:wide-offset-cap]
        assert_equal 1 [r getbit bitmap:roaring:wide-offset-cap $first_wide_bit]
        assert_equal {1} [r bitfield_ro bitmap:roaring:wide-offset-cap GET u1 $first_wide_bit]
        assert_equal 2 [r bitcount bitmap:roaring:wide-offset-cap]

        # Offsets follow the current proto-max-bulk-len exactly like string
        # bitmaps: lowering it below existing data bounds later accesses too.
        r config set proto-max-bulk-len 1048576
        foreach cmd [list \
            [list getbit bitmap:roaring:wide-offset-cap $first_wide_bit] \
            [list bitfield_ro bitmap:roaring:wide-offset-cap GET u1 $first_wide_bit] \
            [list setbit bitmap:roaring:wide-offset-cap $first_wide_bit 1] \
            [list bitfield bitmap:roaring:wide-offset-cap SET u1 $first_wide_bit 1] \
        ] {
            assert_error {*bit offset*out of range*} {r {*}$cmd}
        }
        assert_equal 2 [r bitcount bitmap:roaring:wide-offset-cap]

        r config set bitmap-default-roaring no
        r config set proto-max-bulk-len $oldval
        r del bitmap:roaring:wide-offset-cap
    }

    test {SETBIT keeps the proto-max-bulk-len offset limit on Roaring bitmaps} {
        seed_roaring_bitmap bitmap:roaring:setbit:cap {0}

        assert_error {*bit offset is*out of range*} {
            r setbit bitmap:roaring:setbit:cap 4294967296 1
        }
        assert_equal bitmap [r type bitmap:roaring:setbit:cap]
        assert_equal 1 [r bitcount bitmap:roaring:setbit:cap]
        r del bitmap:roaring:setbit:cap
    }

    test {Roaring bitmap BITCOUNT and BITPOS cover redis-roaring integration cases} {
        seed_roaring_bitmap bitmap:roaring:countpos:fib {1 2 3 5 8 13}
        assert_equal 6 [r bitcount bitmap:roaring:countpos:fib]
        assert_equal 1 [r bitpos bitmap:roaring:countpos:fib 1]
        assert_equal 0 [r bitpos bitmap:roaring:countpos:fib 0]

        seed_roaring_bitmap bitmap:roaring:countpos:first-one {3 4 6 10 12}
        assert_equal 3 [r bitpos bitmap:roaring:countpos:first-one 1]

        seed_roaring_bitmap bitmap:roaring:countpos:first-zero {0 1 2 3 4 6}
        assert_equal 5 [r bitpos bitmap:roaring:countpos:first-zero 0]

        seed_roaring_bitmap bitmap:roaring:countpos:empty {}
        assert_equal -1 [r bitpos bitmap:roaring:countpos:empty 0]
        assert_equal -1 [r bitpos bitmap:roaring:countpos:empty 1]

        seed_roaring_bitmap bitmap:roaring:countpos:single-zero {0}
        assert_equal 1 [r bitpos bitmap:roaring:countpos:single-zero 0]
    }

    test {Roaring bitmap BITCOUNT and BITPOS match string edge ranges} {
        set raw [binary format H* ff00f0800100007f]
        set commands {
            {bitcount key}
            {bitcount key 0 -1}
            {bitcount key 1 4}
            {bitcount key -4 -2}
            {bitcount key 3 44 bit}
            {bitcount key 4 4 bit}
            {bitcount key -20 -1 bit}
            {bitpos key 1}
            {bitpos key 0}
            {bitpos key 1 1 5}
            {bitpos key 0 1 5}
            {bitpos key 1 4 39 bit}
            {bitpos key 0 4 39 bit}
            {bitpos key 0 -8 -1 bit}
        }

        set idx 0
        foreach command $commands {
            assert_roaring_bitmap_command_matches_string "mixed:$idx" $raw $command
            incr idx
        }

        set all_ones [binary format H* ffff]
        foreach command {
            {bitpos key 0}
            {bitpos key 0 0 1}
            {bitpos key 0 1}
            {bitpos key 0 2}
            {bitpos key 0 0 15 bit}
        } {
            assert_roaring_bitmap_command_matches_string "ones:$idx" $all_ones $command
            incr idx
        }
    }

    test {Roaring bitmap BITCOUNT and BITPOS handle container edges} {
        # Bits in distinct 2^16 containers, plus dense runs, exercise the
        # container-walking BITPOS code where uint32 and uint64 arithmetic mix.
        seed_roaring_bitmap bitmap:roaring:cap-edge {0}
        r setbit bitmap:roaring:cap-edge 65535 1
        r setbit bitmap:roaring:cap-edge 65536 1
        r setbit bitmap:roaring:cap-edge 131071 1

        assert_equal 4 [r bitcount bitmap:roaring:cap-edge]
        assert_equal 65535 [r bitpos bitmap:roaring:cap-edge 1 1 -1 bit]
        assert_equal 65535 [r bitpos bitmap:roaring:cap-edge 1 8191]
        assert_equal 131071 [r bitpos bitmap:roaring:cap-edge 1 65537 -1 bit]
        assert_equal 1 [r bitpos bitmap:roaring:cap-edge 0]
        assert_equal -1 [r bitpos bitmap:roaring:cap-edge 0 65535 65535 bit]
        assert_equal 2 [r bitcount bitmap:roaring:cap-edge 65535 65536 bit]
        r del bitmap:roaring:cap-edge

        # A dense run crossing a container boundary: the first clear bit
        # after the run must come from the container-level scan. With an
        # explicit BIT range every bit is set, so the reply is -1; without an
        # explicit end the logical length supplies the imaginary trailing
        # zero at bit 65568.
        seed_roaring_bitmap bitmap:roaring:run-edge {}
        r bitfield bitmap:roaring:run-edge SET i64 65504 -1
        assert_equal {-1} [r bitfield_ro bitmap:roaring:run-edge GET i64 65504]
        assert_equal 65504 [r bitpos bitmap:roaring:run-edge 1]
        assert_equal -1 [r bitpos bitmap:roaring:run-edge 0 65504 -1 bit]
        assert_equal 65568 [r bitpos bitmap:roaring:run-edge 0 8188]
        assert_equal 64 [r bitcount bitmap:roaring:run-edge]
        r del bitmap:roaring:run-edge
    }

    test {BITFIELD writes Roaring bitmap values through the direct write path} {
        r set bitmap:roaring:bitfield [binary format H* 00]
        convert_string_bitmap_to_roaring r bitmap:roaring:bitfield
        assert_equal {0 15} [r bitfield bitmap:roaring:bitfield SET u4 4 15 GET u8 0]
        assert_equal bitmap [r type bitmap:roaring:bitfield]
        assert_equal bitmap-roaring [r object encoding bitmap:roaring:bitfield]
        assert_equal [binary format H* 0f] [r debug bitmap-raw bitmap:roaring:bitfield]
        assert_equal {15} [r bitfield_ro bitmap:roaring:bitfield GET u4 4]

        assert_equal {0} [r bitfield bitmap:roaring:bitfield SET u1 23 0]
        assert_equal bitmap [r type bitmap:roaring:bitfield]
        assert_equal [binary format H* 0f0000] [r debug bitmap-raw bitmap:roaring:bitfield]

        seed_roaring_bitmap bitmap:roaring:bitfield:clear {0}
        assert_equal {2} [r bitfield bitmap:roaring:bitfield:clear SET u2 0 0]
        assert_equal 0 [r bitcount bitmap:roaring:bitfield:clear]
        assert_equal [binary format H* 00] [r debug bitmap-raw bitmap:roaring:bitfield:clear]
    }

    test {BITFIELD signed INCRBY preserves Roaring bitmap values} {
        seed_roaring_bitmap bitmap:roaring:bitfield:signed {}

        assert_equal {0 1 1} [r bitfield bitmap:roaring:bitfield:signed SET i5 0 -1 INCRBY i5 0 2 GET i5 0]
        assert_equal bitmap [r type bitmap:roaring:bitfield:signed]
        assert_equal bitmap-roaring [r object encoding bitmap:roaring:bitfield:signed]
        assert_equal {1} [r bitfield_ro bitmap:roaring:bitfield:signed GET i5 0]
    }

    test {Roaring bitmap BITFIELD direct paths match string edge cases} {
        set raw [binary format H* 0102030400]
        set commands {
            {bitfield key GET u4 0 GET i6 9 GET u12 17}
            {bitfield_ro key GET u4 0 GET i6 9 GET u12 17}
            {bitfield key SET u5 3 17 GET u13 0 SET i6 16 -8 GET i6 16}
            {bitfield key INCRBY u8 4 7 GET u12 0}
            {bitfield key OVERFLOW SAT INCRBY i5 9 20 GET i5 9}
            {bitfield key OVERFLOW WRAP INCRBY u4 #1 20 GET u8 0}
            {bitfield key OVERFLOW FAIL SET u2 10 5 GET u2 10}
            {bitfield key SET u1 47 0 GET u1 47}
        }

        set idx 0
        foreach command $commands {
            assert_roaring_bitmap_write_matches_string $idx $raw $command
            incr idx
        }

        assert_roaring_bitmap_write_matches_string grow-after-failed-high-write \
            [binary format H* 00] \
            {bitfield key SET u1 0 1 OVERFLOW FAIL SET u2 47 5}

        assert_roaring_bitmap_write_matches_string grow-after-fail-only-high-write \
            [binary format H* 00] \
            {bitfield key OVERFLOW FAIL SET u2 47 5}

        set fail_key bitmap:roaring:bitfield:overflow-fail-string-growth
        r config set bitmap-default-roaring no
        r set $fail_key [binary format H* 00]
        assert_equal string [r type $fail_key]
        assert_equal {{}} [r bitfield $fail_key OVERFLOW FAIL SET u2 47 5]
        assert_equal string [r type $fail_key]
        assert_equal 7 [r strlen $fail_key]
        assert_equal [binary format H* 00000000000000] [r get $fail_key]

        set watched_fail_key bitmap:roaring:bitfield:overflow-fail-string-growth-watch
        r set $watched_fail_key [binary format H* 00]
        r watch $watched_fail_key
        assert_equal {{}} [r bitfield $watched_fail_key OVERFLOW FAIL SET u2 47 5]
        r multi
        r ping
        assert_equal {} [r exec]
        assert_equal 7 [r strlen $watched_fail_key]
    }

    test {BITFIELD uses the same offset limit for string and Roaring bitmaps} {
        set limit 1048576
        set oldval [config_get_set proto-max-bulk-len $limit]
        set limit_bits [expr {$limit * 8}]
        set last_allowed [expr {$limit_bits - 1}]

        seed_string_bitmap bitmap:string:bitfield:limit {}
        seed_roaring_bitmap bitmap:roaring:bitfield:limit {}

        foreach key {bitmap:string:bitfield:limit bitmap:roaring:bitfield:limit} {
            assert_error {*bit offset*out of range*} {
                r bitfield $key SET u1 $limit_bits 1
            }
            assert_error {*bit offset*out of range*} {
                r bitfield_ro $key GET u1 $limit_bits
            }
            assert_error {*bit offset*out of range*} {
                r bitfield $key GET u1 $limit_bits SET u1 0 0
            }
            # An op whose offset itself passes the limit may span up to 63
            # bits past it, matching historical string bitmap behavior.
            assert_equal {0} [r bitfield $key SET u2 $last_allowed 3]
            assert_equal 2 [r bitcount $key]
        }
        assert_equal string [r type bitmap:string:bitfield:limit]
        assert_equal bitmap [r type bitmap:roaring:bitfield:limit]
        assert_equal bitmap-roaring [r object encoding bitmap:roaring:bitfield:limit]

        r config set proto-max-bulk-len $oldval
        r del bitmap:string:bitfield:limit bitmap:roaring:bitfield:limit
    }

    test {translated redis-roaring int-array bit-array and clear scenarios use core bitmap commands} {
        r config set bitmap-default-roaring yes

        set int_key bitmap:roaring:translated:int-array
        r del $int_key
        foreach bit {1 2 3 4 5} {
            assert_equal 0 [r setbit $int_key $bit 1]
        }
        assert_equal bitmap [r type $int_key]
        assert_bitmap_has_exact_bits $int_key {1 2 3 4 5}

        foreach bit {1 3} {
            assert_equal 1 [r setbit $int_key $bit 0]
        }
        assert_bitmap_has_exact_bits $int_key {2 4 5}
        assert_equal 2 [r bitcount $int_key 4 5 bit]

        foreach bit {4 5} {
            assert_equal 1 [r setbit $int_key $bit 0]
        }
        assert_bitmap_has_exact_bits $int_key {2}

        set range_key bitmap:roaring:translated:range-array
        r del $range_key
        foreach bit {0 8 16} {
            assert_equal 0 [r setbit $range_key $bit 1]
        }
        assert_bitmap_has_exact_bits $range_key {0 8 16}
        assert_equal {1 1 1} [r bitfield_ro $range_key GET u1 0 GET u1 8 GET u1 16]
        assert_equal 3 [r bitcount $range_key 0 16 bit]

        set bitarray_key bitmap:roaring:translated:bit-array
        r del $bitarray_key
        foreach bit {1 2 4 7 11 14 17 18 21} {
            assert_equal 0 [r setbit $bitarray_key $bit 1]
        }
        assert_bitmap_has_exact_bits $bitarray_key {1 2 4 7 11 14 17 18 21}
        assert_equal {0 1 0 1} [r bitfield_ro $bitarray_key GET u1 0 GET u1 1 GET u1 24 GET u1 21]

        r config set bitmap-default-roaring no
    }

    test {translated redis-roaring range full min and max scenarios use core bitmap commands} {
        r config set bitmap-default-roaring yes

        set range_key bitmap:roaring:translated:setrange
        r del $range_key
        for {set bit 0} {$bit < 5} {incr bit} {
            assert_equal 0 [r setbit $range_key $bit 1]
        }
        assert_bitmap_has_exact_bits $range_key {0 1 2 3 4}
        assert_equal 0 [r bitpos $range_key 1]
        assert_equal 5 [r bitpos $range_key 0]

        set full_key bitmap:roaring:translated:setfull
        r set $full_key [binary format H* ff]
        convert_string_bitmap_to_roaring r $full_key
        assert_equal bitmap [r type $full_key]
        assert_equal bitmap-roaring [r object encoding $full_key]
        assert_bitmap_has_exact_bits $full_key {0 1 2 3 4 5 6 7}
        assert_equal 8 [r bitpos $full_key 0]

        set minmax_key bitmap:roaring:translated:minmax
        seed_roaring_bitmap $minmax_key {}
        assert_equal 0 [r bitcount $minmax_key]
        assert_equal -1 [r bitpos $minmax_key 1]

        assert_equal 0 [r setbit $minmax_key 100 1]
        assert_bitmap_has_exact_bits $minmax_key {100}
        assert_equal 100 [r bitpos $minmax_key 1]

        assert_equal 0 [r setbit $minmax_key 0 1]
        assert_bitmap_has_exact_bits $minmax_key {0 100}
        assert_equal 0 [r bitpos $minmax_key 1]
        assert_equal 1 [r getbit $minmax_key 100]

        assert_equal 1 [r setbit $minmax_key 0 0]
        assert_equal 1 [r setbit $minmax_key 100 0]
        assert_equal 0 [r bitcount $minmax_key]
        assert_equal -1 [r bitpos $minmax_key 1]

        assert {[r memory usage $full_key] > 0}
        r config set bitmap-default-roaring no
    }

    test {translated redis-roaring contains and jaccard scenarios use bitmap algebra} {
        set a bitmap:roaring:translated:contains:a
        set b bitmap:roaring:translated:contains:b
        set c bitmap:roaring:translated:contains:c
        set e bitmap:roaring:translated:contains:empty

        seed_roaring_bitmap $a {1 2 3 4 5}
        seed_roaring_bitmap $b {2 3}
        seed_roaring_bitmap $c {3 4 6}
        seed_roaring_bitmap $e {}

        r bitop and bitmap:roaring:translated:contains:some $a $b
        assert_equal 2 [r bitcount bitmap:roaring:translated:contains:some]

        r bitop and bitmap:roaring:translated:contains:none $a bitmap:roaring:translated:contains:missing
        assert_equal 0 [r bitcount bitmap:roaring:translated:contains:none]

        r bitop diff bitmap:roaring:translated:contains:subset-miss $b $a
        assert_equal 0 [r bitcount bitmap:roaring:translated:contains:subset-miss]
        assert {[r bitcount $b] < [r bitcount $a]}

        r bitop diff bitmap:roaring:translated:contains:not-subset $c $a
        assert_bitmap_has_exact_bits bitmap:roaring:translated:contains:not-subset {6}

        seed_roaring_bitmap bitmap:roaring:translated:contains:eq1 {1 2 3 4 5}
        seed_roaring_bitmap bitmap:roaring:translated:contains:eq2 {1 2 3 4 5}
        r bitop xor bitmap:roaring:translated:contains:eq-diff \
            bitmap:roaring:translated:contains:eq1 bitmap:roaring:translated:contains:eq2
        assert_equal 0 [r bitcount bitmap:roaring:translated:contains:eq-diff]

        r bitop diff bitmap:roaring:translated:contains:empty-subset $e $a
        assert_equal 0 [r bitcount bitmap:roaring:translated:contains:empty-subset]

        assert_bitmap_translated_jaccard overlap {1 2 3 4 5} {3 4 5 6 7} 3 7 0.428571
        assert_bitmap_translated_jaccard subset {1 2 3} {1 2 3 4 5} 3 5 0.600000
        assert_bitmap_translated_jaccard identical {8 13 21} {8 13 21} 3 3 1.000000
        assert_bitmap_translated_jaccard one-empty {1 2 3} {} 0 3 0.000000
        assert_bitmap_translated_jaccard disjoint {1 2} {3 4} 0 4 0.000000
        assert_bitmap_translated_jaccard empty {} {} 0 0 -1
    }

    test {BITOP stores roaring destinations when sources include Roaring bitmaps} {
        r set bitmap:roaring:bitop:a [binary format H* f000]
        convert_string_bitmap_to_roaring r bitmap:roaring:bitop:a
        r set bitmap:roaring:bitop:b [binary format H* 0fff]
        r set bitmap:roaring:bitop:dest [binary format H* aa]
        convert_string_bitmap_to_roaring r bitmap:roaring:bitop:dest
        assert_equal 2 [r bitop or bitmap:roaring:bitop:dest bitmap:roaring:bitop:a bitmap:roaring:bitop:b]
        assert_equal bitmap [r type bitmap:roaring:bitop:dest]
        assert_equal [binary format H* ffff] [r debug bitmap-raw bitmap:roaring:bitop:dest]

        assert_equal 2 [r bitop not bitmap:roaring:bitop:not bitmap:roaring:bitop:a]
        assert_equal bitmap [r type bitmap:roaring:bitop:not]
        assert_equal [binary format H* 0fff] [r debug bitmap-raw bitmap:roaring:bitop:not]
    }

    test {BITOP NOT allows Roaring bitmap sources larger than proto-max-bulk-len} {
        set limit 1048576
        set oldval [config_get_set proto-max-bulk-len [expr {$limit + 1}]]
        r config set bitmap-default-roaring yes
        r del bitop:not:roaring:limit bitop:not:roaring:too-big \
            bitop:not:roaring:out bitop:not:roaring:sentinel

        assert_equal 0 [r setbit bitop:not:roaring:limit [expr {$limit * 8 - 1}] 1]
        assert_equal 0 [r setbit bitop:not:roaring:too-big [expr {($limit + 1) * 8 - 1}] 1]
        assert_equal bitmap [r type bitop:not:roaring:limit]
        assert_equal bitmap [r type bitop:not:roaring:too-big]

        r config set proto-max-bulk-len $limit
        assert_equal $limit [r bitop not bitop:not:roaring:out bitop:not:roaring:limit]
        assert_equal bitmap [r type bitop:not:roaring:out]
        assert_equal 1 [r getbit bitop:not:roaring:out [expr {$limit * 8 - 2}]]
        assert_equal 0 [r getbit bitop:not:roaring:out [expr {$limit * 8 - 1}]]

        # A source above the lowered limit is no longer rejected; the NOT
        # succeeds and overwrites the destination with the complement.
        r set bitop:not:roaring:sentinel keep
        assert_equal [expr {$limit + 1}] \
            [r bitop not bitop:not:roaring:sentinel bitop:not:roaring:too-big]
        assert_equal bitmap [r type bitop:not:roaring:sentinel]
        assert_equal 1 [r getbit bitop:not:roaring:sentinel 0]
        assert_equal bitmap [r type bitop:not:roaring:too-big]

        r config set bitmap-default-roaring no
        r config set proto-max-bulk-len $oldval
        # Restore the limit before reading the high bit (GETBIT caps its offset
        # at proto-max-bulk-len too).
        assert_equal 0 [r getbit bitop:not:roaring:sentinel [expr {($limit + 1) * 8 - 1}]]
        r del bitop:not:roaring:limit bitop:not:roaring:too-big \
            bitop:not:roaring:out bitop:not:roaring:sentinel
    }

    test {BITOP NOT bounds missing Roaring chunk amplification} {
        set missing_chunk_limit 65536
        set chunk_bytes [expr {1 << 13}]
        set allocation_envelope [expr {$missing_chunk_limit * $chunk_bytes}]
        set byte_len [expr {$allocation_envelope + 1}]
        set last_bit [expr {$byte_len * 8 - 1}]
        set missing_chunk_error \
            {ERR BITOP NOT would materialize more than 65536 missing Roaring chunks}
        r config set proto-max-bulk-len $byte_len
        r config set bitmap-default-roaring yes
        r del bitop:not:roaring:huge bitop:not:roaring:huge:dest \
            bitop:not:roaring:huge:copy bitop:not:roaring:limit \
            bitop:not:roaring:limit:dest bitop:not:roaring:dense:dest

        # An empty source at exactly 65,536 missing chunks remains valid, even
        # under a limit low enough that the fixed floor is the whole budget.
        set limit_last_bit [expr {$allocation_envelope * 8 - 1}]
        assert_equal 0 [r setbit bitop:not:roaring:limit $limit_last_bit 0]
        r config set proto-max-bulk-len 1048576
        assert_equal $allocation_envelope \
            [r bitop not bitop:not:roaring:limit:dest \
                bitop:not:roaring:limit]
        r config set proto-max-bulk-len $byte_len
        assert_equal 1 [r getbit bitop:not:roaring:limit:dest 0]
        assert_equal 1 [r getbit bitop:not:roaring:limit:dest $limit_last_bit]

        # The byte-length envelope is not itself a limit. The full result above
        # has a present container in every chunk; extending it by one zero byte
        # leaves only one missing chunk, so complementing it remains cheap.
        assert_equal 0 [r setbit bitop:not:roaring:limit:dest $last_bit 0]
        assert_equal $byte_len [r bitop not bitop:not:roaring:dense:dest \
            bitop:not:roaring:limit:dest]
        assert_equal 0 [r getbit bitop:not:roaring:dense:dest 0]
        assert_equal 1 [r getbit bitop:not:roaring:dense:dest $last_bit]
        assert_equal 8 [r bitcount bitop:not:roaring:dense:dest]
        assert_lessthan [r memory usage bitop:not:roaring:dense:dest] 65536
        r del bitop:not:roaring:limit bitop:not:roaring:limit:dest
        r del bitop:not:roaring:dense:dest

        # SETBIT 0 extends the logical length without allocating a container.
        # DUMP/RESTORE preserves that length in a compact portable payload even
        # after the client-visible offset limit is lowered.
        assert_equal 0 [r setbit bitop:not:roaring:huge $last_bit 0]
        set payload [r dump bitop:not:roaring:huge]
        assert_lessthan [string length $payload] 64
        r del bitop:not:roaring:huge
        r config set bitmap-default-roaring no
        r config set proto-max-bulk-len 1048576
        r restore bitop:not:roaring:huge 0 $payload
        assert_equal bitmap [r type bitop:not:roaring:huge]
        assert_lessthan [r memory usage bitop:not:roaring:huge] 65536

        r set bitop:not:roaring:huge:dest keep
        set dirty [s rdb_changes_since_last_save]
        assert_error $missing_chunk_error {
            r bitop not bitop:not:roaring:huge:dest bitop:not:roaring:huge
        }
        assert_equal keep [r get bitop:not:roaring:huge:dest]
        assert_equal $dirty [s rdb_changes_since_last_save]

        # Aliasing is rejected before the source can be replaced. Other BITOPs
        # retain wide sparse support and preserve the compact logical length.
        assert_error $missing_chunk_error {
            r bitop not bitop:not:roaring:huge bitop:not:roaring:huge
        }
        assert_equal $byte_len [r bitop or bitop:not:roaring:huge:copy \
            bitop:not:roaring:huge]
        assert_equal bitmap [r type bitop:not:roaring:huge:copy]
        assert_equal 0 [r bitcount bitop:not:roaring:huge:copy]
        assert_lessthan [r memory usage bitop:not:roaring:huge:copy] 65536

        r del bitop:not:roaring:huge bitop:not:roaring:huge:dest \
            bitop:not:roaring:huge:copy
        set _ {}
    } {} {config:restore}

    test {BITOP NOT complements Roaring values BITFIELD extends past proto-max-bulk-len} {
        # BITFIELD checks only a field's first bit against proto-max-bulk-len,
        # so a field at the last allowed offset extends the logical length up
        # to 8 bytes further. At the default limit that spans one chunk more
        # than the fixed 65,536-chunk floor. The string form of the same value
        # complements fine, so the Roaring form must too.
        set limit 536870912
        set last_offset [expr {$limit * 8 - 1}]
        r config set proto-max-bulk-len $limit
        r config set bitmap-default-roaring yes
        r del bitop:not:bitfield:src bitop:not:bitfield:dest

        assert_equal 0 [r bitfield bitop:not:bitfield:src set u8 $last_offset 0]
        assert_equal bitmap [r type bitop:not:bitfield:src]
        assert_equal 0 [r bitcount bitop:not:bitfield:src]

        # Same replies as the string form: its length and every bit set. The
        # 512 MiB of set bits stays compact as full run containers.
        assert_equal [expr {$limit + 1}] \
            [r bitop not bitop:not:bitfield:dest bitop:not:bitfield:src]
        assert_equal bitmap [r type bitop:not:bitfield:dest]
        assert_equal [expr {($limit + 1) * 8}] \
            [r bitcount bitop:not:bitfield:dest]
        assert_lessthan [r memory usage bitop:not:bitfield:dest] \
            [expr {16 * 1024 * 1024}]

        # A 64-bit field reaches the full 8-byte overshoot. With a limit 7
        # bytes short of a chunk boundary, only the full overshoot crosses
        # into the next chunk.
        set limit [expr {536870912 + 8192 - 7}]
        set last_offset [expr {$limit * 8 - 1}]
        r config set proto-max-bulk-len $limit
        assert_equal 0 [r bitfield bitop:not:bitfield:src set i64 $last_offset 0]
        assert_equal [expr {$limit + 8}] \
            [r bitop not bitop:not:bitfield:dest bitop:not:bitfield:src]
        assert_equal [expr {($limit + 8) * 8}] \
            [r bitcount bitop:not:bitfield:dest]
        assert_lessthan [r memory usage bitop:not:bitfield:dest] \
            [expr {16 * 1024 * 1024}]

        r del bitop:not:bitfield:src bitop:not:bitfield:dest
        set _ {}
    } {} {config:restore}

    test {BITOP NOT missing-chunk budget follows proto-max-bulk-len} {
        # A raised limit lets clients create longer values, which stay
        # complementable like their string forms. Once the limit is lowered
        # again, the same value is bounded by the budget of the new limit.
        set limit [expr {1024 * 1024 * 1024}]
        r config set proto-max-bulk-len $limit
        r config set bitmap-default-roaring yes
        r del bitop:not:raised:src bitop:not:raised:dest

        assert_equal 0 [r setbit bitop:not:raised:src [expr {$limit * 8 - 1}] 0]
        assert_equal $limit \
            [r bitop not bitop:not:raised:dest bitop:not:raised:src]
        assert_equal [expr {$limit * 8}] [r bitcount bitop:not:raised:dest]
        assert_lessthan [r memory usage bitop:not:raised:dest] \
            [expr {32 * 1024 * 1024}]

        r config set proto-max-bulk-len 536870912
        r set bitop:not:raised:dest keep
        assert_error {ERR BITOP NOT would materialize more than 65537 missing Roaring chunks} {
            r bitop not bitop:not:raised:dest bitop:not:raised:src
        }
        # A script run by a normal client is bounded too.
        assert_error {*BITOP NOT would materialize more than 65537 missing Roaring chunks*} {
            r eval {return redis.call('bitop', 'not', KEYS[1], KEYS[2])} 2 \
                bitop:not:raised:dest bitop:not:raised:src
        }
        assert_equal keep [r get bitop:not:raised:dest]

        r del bitop:not:raised:src bitop:not:raised:dest
        set _ {}
    } {} {config:restore}

    test {BITOP NOT keeps sparse per-chunk complements compact while building} {
        set chunks 512
        set chunk_bits [expr {1 << 16}]
        set bit_len [expr {$chunks * $chunk_bits}]
        set byte_len [expr {$bit_len / 8}]
        r config set bitmap-default-roaring yes
        r del bitop:not:roaring:arrays bitop:not:roaring:arrays:dest

        # One set bit in every logical chunk creates compact ARRAY containers.
        # Extending the final byte with zero makes every chunk full-length.
        set source_bits {}
        for {set chunk 0} {$chunk < $chunks} {incr chunk} {
            set within [lindex {0 32768 65535} [expr {$chunk % 3}]]
            set source_bit [expr {$chunk * $chunk_bits + $within}]
            lappend source_bits $source_bit
            assert_equal 0 [r setbit bitop:not:roaring:arrays \
                $source_bit 1]
        }
        assert_equal 0 [r setbit bitop:not:roaring:arrays \
            [expr {$bit_len - 1}] 0]
        assert_equal $chunks [r bitcount bitop:not:roaring:arrays]
        assert_lessthan [r memory usage bitop:not:roaring:arrays] 1048576

        set track_allocations [expr {
            [string match {*jemalloc*} [s mem_allocator]] &&
            ![catch {r debug mallctl thread.allocated} allocated_before]
        }]
        assert_equal $byte_len [r bitop not bitop:not:roaring:arrays:dest \
            bitop:not:roaring:arrays]
        if {$track_allocations} {
            set allocated_after [r debug mallctl thread.allocated]
            assert_lessthan [expr {$allocated_after - $allocated_before}] \
                [expr {2 * 1024 * 1024}]
        }

        assert_equal 0 [r getbit bitop:not:roaring:arrays:dest 0]
        assert_equal 1 [r getbit bitop:not:roaring:arrays:dest 1]
        assert_equal 1 [r getbit bitop:not:roaring:arrays:dest \
            [expr {$bit_len - 1}]]
        assert_equal [expr {$bit_len - $chunks}] \
            [r bitcount bitop:not:roaring:arrays:dest]
        assert_lessthan [r memory usage bitop:not:roaring:arrays:dest] 1048576

        set reads [list bitfield_ro bitop:not:roaring:arrays:dest]
        foreach source_bit $source_bits {
            lappend reads get u1 $source_bit
        }
        assert_equal [lrepeat $chunks 0] [r {*}$reads]

        r del bitop:not:roaring:arrays bitop:not:roaring:arrays:dest
        set _ {}
    } {} {config:restore}

    test {BITOP NOT complements every Roaring container type and a partial tail} {
        set raw [binary format H* 80]
        append raw [string repeat [binary format H* 00] 8191]
        append raw [string repeat [binary format H* ff] 8192]
        append raw [string repeat [binary format H* aa] 8192]
        append raw [binary format H* 80]
        append raw [string repeat [binary format H* 00] 16]

        r del bitop:not:containers:string bitop:not:containers:roaring \
            bitop:not:containers:string:dest bitop:not:containers:roaring:dest
        r set bitop:not:containers:string $raw
        r set bitop:not:containers:roaring $raw
        convert_string_bitmap_to_roaring r bitop:not:containers:roaring

        set string_len [r bitop not bitop:not:containers:string:dest \
            bitop:not:containers:string]
        set roaring_len [r bitop not bitop:not:containers:roaring:dest \
            bitop:not:containers:roaring]
        assert_equal $string_len $roaring_len
        assert_equal $string_len [string length $raw]
        assert_equal [r get bitop:not:containers:string:dest] \
            [r debug bitmap-raw bitop:not:containers:roaring:dest]
        assert_equal bitmap [r type bitop:not:containers:roaring:dest]

        r del bitop:not:containers:string bitop:not:containers:roaring \
            bitop:not:containers:string:dest bitop:not:containers:roaring:dest
    }

    test {BITOP NOT avoids RUN churn for fragmented ARRAY containers} {
        set chunks 256
        set chunk_bytes 8192
        set source_cardinality [expr {$chunks * 2048}]
        set raw_chunk [string repeat [binary format H* 80000000] 2048]
        set raw [string repeat $raw_chunk $chunks]
        r del bitop:not:fragmented bitop:not:fragmented:dest
        r set bitop:not:fragmented $raw
        convert_string_bitmap_to_roaring r bitop:not:fragmented
        unset raw raw_chunk

        assert_equal $source_cardinality [r bitcount bitop:not:fragmented]
        assert_lessthan [r memory usage bitop:not:fragmented] \
            [expr {2 * 1024 * 1024}]
        set track_allocations [expr {
            [string match {*jemalloc*} [s mem_allocator]] &&
            ![catch {r debug mallctl thread.allocated} allocated_before]
        }]

        set byte_len [expr {$chunks * $chunk_bytes}]
        assert_equal $byte_len [r bitop not bitop:not:fragmented:dest \
            bitop:not:fragmented]
        if {$track_allocations} {
            set allocated_after [r debug mallctl thread.allocated]
            assert_lessthan [expr {$allocated_after - $allocated_before}] \
                [expr {6 * 1024 * 1024}]
        }
        assert_equal [expr {$byte_len * 8 - $source_cardinality}] \
            [r bitcount bitop:not:fragmented:dest]
        assert_lessthan [r memory usage bitop:not:fragmented:dest] \
            [expr {4 * 1024 * 1024}]

        r del bitop:not:fragmented bitop:not:fragmented:dest
    }

    test {non-NOT Roaring BITOP survives lowering proto-max-bulk-len} {
        set limit 1048576
        set oldval [config_get_set proto-max-bulk-len [expr {$limit + 1}]]
        set last_bit [expr {($limit + 1) * 8 - 1}]

        r config set bitmap-default-roaring yes
        assert_equal 0 [r setbit bitop:limit:roaring $last_bit 1]
        r config set bitmap-default-roaring no
        assert_equal 0 [r setbit bitop:limit:string $last_bit 1]

        r config set proto-max-bulk-len $limit
        assert_equal [expr {$limit + 1}] \
            [r bitop or bitop:limit:roaring:out bitop:limit:roaring]
        assert_equal [expr {$limit + 1}] \
            [r bitop or bitop:limit:string:out bitop:limit:string]
        assert_equal bitmap [r type bitop:limit:roaring:out]
        assert_equal string [r type bitop:limit:string:out]
        assert_equal 1 [r bitcount bitop:limit:roaring:out]

        r config set proto-max-bulk-len [expr {$limit + 1}]
        assert_equal [r get bitop:limit:string:out] \
            [r debug bitmap-raw bitop:limit:roaring:out]

        r config set proto-max-bulk-len $oldval
        r del bitop:limit:roaring bitop:limit:string \
            bitop:limit:roaring:out bitop:limit:string:out
    }

    if {[s arch_bits] == 64} {
        test {Roaring BITOP keeps source lengths above 4 GiB} {
            # 64-bit coverage for BITOP over a source longer than 4 GiB, whose
            # length a 32-bit size_t would wrap to 2. 32-bit builds cannot hold
            # such a source (see the AOF replay test above), so it never runs
            # there; BITOP keeps source lengths as uint64_t for defense in depth.
            set bit [expr {(1 << 35) + 8}]
            set byte_len [expr {($bit >> 3) + 1}]
            set oldval [config_get_set proto-max-bulk-len $byte_len]

            create_roaring_bitmap_from_bits r bitop:wide:roaring [list $bit]
            create_roaring_bitmap_from_bits r bitop:wide:small {0}
            r set bitop:wide:string [binary format H* 80]
            foreach op {and or xor diff diff1 andor one} {
                assert_equal $byte_len [r bitop $op bitop:wide:out \
                    bitop:wide:string bitop:wide:roaring bitop:wide:small]
                assert_equal bitmap [r type bitop:wide:out]
            }

            assert_equal $byte_len [r bitop or bitop:wide:out bitop:wide:roaring]
            assert_equal 1 [r bitcount bitop:wide:out]
            assert_equal $bit [r bitpos bitop:wide:out 1]
            assert_equal [expr {$byte_len * 8 - 1}] \
                [r bitpos bitop:wide:out 0 -1 -1 bit]

            r config set proto-max-bulk-len $oldval
            r del bitop:wide:roaring bitop:wide:small bitop:wide:string \
                bitop:wide:out
        }
    }

    test {BITOP Roaring bitmap sources match string bitmap results for all operations} {
        set a {0 4 5 6 20}
        set b {1 5 6 21}
        set c {2 3 5 6 7 20}

        assert_roaring_bitop_matches_string and AND [list $a $b $c]
        assert_roaring_bitop_matches_string or OR [list $a $b $c]
        assert_roaring_bitop_matches_string xor XOR [list $a $b $c]
        assert_roaring_bitop_matches_string diff DIFF [list $a $b $c]
        assert_roaring_bitop_matches_string diff1 DIFF1 [list $a $b $c]
        assert_roaring_bitop_matches_string andor ANDOR [list $a $b $c]
        assert_roaring_bitop_matches_string one ONE [list $a $b $c]
        assert_roaring_bitop_matches_string not NOT [list $a]
    }

    test {BITOP current operations cover redis-roaring algebra cases} {
        set cases {
            {diff:missing-all diff {{} {}} {} {0 1}}
            {diff:basic diff {{1 2 3 4 5} {3 4 5 6 7}} {1 2}}
            {diff:multi-subtract diff {{1 2 3 4 5 6 7 8} {2 3} {5 6}} {1 4 7 8}}
            {diff:three-subtractors diff {{1 2 3 4 5 6 7 8 9 10} {1 2} {3 4} {5 6}} {7 8 9 10}}
            {diff:subset diff {{1 2 3} {1 2 3 4 5}} {}}
            {diff:disjoint diff {{1 2 3} {7 8 9}} {1 2 3}}
            {diff:missing-first diff {{} {1 2 3}} {} {0}}
            {diff:missing-subtractor diff {{1 2 3 4} {}} {1 2 3 4} {1}}
            {diff:overlap-subtractors diff {{1 2 3 4 5 6} {2 3 4} {3 4 5}} {1 6}}
            {diff:overwrite-dest diff {{5 6 7} {6}} {5 7} {} -1 {99 100}}
            {diff:large-values diff {{1000 2000 3000 4000} {2000 3000}} {1000 4000}}
            {diff:chained-equivalent diff {{1 2 3 4 5} {3 4} {1}} {2 5}}
            {diff:dest-first-source diff {{1 2 3 4 5 6} {3 4 5}} {1 2 6} {} 0}
            {diff:dest-middle-source diff {{1 2 3 4 5 6 7 8} {2 3 4} {6 7}} {1 5 8} {} 1}
            {diff:dest-last-source diff {{10 20 30 40 50} {20 30} {40}} {10 50} {} 2}
            {diff:dest-first-empty-result diff {{7 8 9} {7 8 9 10 11}} {} {} 0}

            {diff1:missing-all diff1 {{} {}} {} {0 1}}
            {diff1:basic diff1 {{3 4 5} {1 2 3 4 5 6 7}} {1 2 6 7}}
            {diff1:multi-source diff1 {{2 3 5 6} {1 2 3 4} {5 6 7 8}} {1 4 7 8}}
            {diff1:three-sources diff1 {{1 2 5 6 9 10} {1 2 3} {4 5 6} {7 8 9}} {3 4 7 8}}
            {diff1:subset diff1 {{1 2 3 4 5} {2 3 4}} {}}
            {diff1:disjoint diff1 {{1 2 3} {7 8 9}} {7 8 9}}
            {diff1:missing-first diff1 {{} {5 6 7 8}} {5 6 7 8} {0}}
            {diff1:missing-y diff1 {{1 2 3 4} {}} {} {1}}
            {diff1:all-y-missing diff1 {{10 20 30} {} {}} {} {1 2}}
            {diff1:overlap-y diff1 {{3 4 5} {1 2 3 4} {4 5 6 7}} {1 2 6 7}}
            {diff1:overwrite-dest diff1 {{5 6} {5 6 7 8}} {7 8} {} -1 {99 100}}
            {diff1:large-values diff1 {{2000 3000} {1000 2000 3000 4000}} {1000 4000}}
            {diff1:chained-equivalent diff1 {{1} {1 2 5}} {2 5}}
            {diff1:dest-x-source diff1 {{3 4 5} {1 2 3 4 5 6}} {1 2 6} {} 0}
            {diff1:dest-first-y diff1 {{2 3 4} {1 2 3 4 5 6} {6 7 8}} {1 5 6 7 8} {} 1}
            {diff1:dest-middle-y diff1 {{5 10 15} {1 5 10} {10 15 20} {15 20 25}} {1 20 25} {} 2}
            {diff1:dest-last-y diff1 {{20 30} {10 20 30} {30 40 50}} {10 40 50} {} 2}
            {diff1:dest-y-empty-result diff1 {{7 8 9 10 11} {7 8 9}} {} {} 1}
            {diff1:equal-x-y diff1 {{100 200 300} {100 200 300}} {}}
            {diff1:y-union-equals-x diff1 {{1 2 3 4 5 6} {1 2 3} {4 5 6}} {}}
            {diff1:four-y diff1 {{5 10 15 20 25 30} {1 5} {10 11} {15 16} {20 21}} {1 11 16 21}}

            {andor:basic andor {{1 2 3 4} {3 4 5 6}} {3 4}}
            {andor:three andor {{1 2 3} {2 3 4} {3 4 5}} {2 3}}
            {andor:disjoint andor {{1 2} {3 4} {5 6}} {}}
            {andor:missing-middle andor {{1 2 3} {} {2 3 4}} {2 3} {1}}
            {andor:missing-first andor {{} {1 2 3} {2 3 4}} {} {0}}
            {andor:many andor {{1 2 3 4 5} {2 3} {3 4} {4 5} {5 6} {6 7} {7 8} {8 9} {9 10} {10 11}} {2 3 4 5}}
            {andor:overwrite-dest andor {{1 2} {1}} {1} {} -1 {100 200}}
            {andor:dest-first-source andor {{1 2 3 10 20} {2 3 4 10 30} {3 4 5 10 40}} {2 3 10} {} 0}

            {one:single one {{1 3 5}} {1 3 5}}
            {one:non-overlap one {{1 3 5} {2 4 6}} {1 2 3 4 5 6}}
            {one:overlap one {{1 2 3} {3 4 5}} {1 2 4 5}}
            {one:three one {{0 4 5 6} {1 5 6} {2 3 5 6 7}} {0 1 2 3 4 7}}
            {one:all-same one {{10 20 30} {10 20 30} {10 20 30}} {}}
            {one:missing-middle one {{1 2 3} {} {3 4 5}} {1 2 4 5} {1}}
            {one:complex-overlap one {{1 2 3 4 5} {2 3 4 6 7} {3 4 5 7 8} {4 5 6 8 9}} {1 9}}
            {one:large-values one {{1000000 2000000} {2000000 3000000}} {1000000 3000000}}
            {one:overwrite-dest one {{1 2} {2 3}} {1 3} {} -1 {100 200 300}}
        }

        foreach case $cases {
            set missing_indexes {}
            set alias_index -1
            set dest_seed __none__
            lassign $case name op sources expected missing_indexes alias_index dest_seed
            assert_roaring_bitop_bitset_case $name $op $sources $expected $missing_indexes $alias_index $dest_seed
        }
    }

    test {BITOP current operation syntax errors are preserved on roaring paths} {
        seed_roaring_bitmap bitmap:roaring:bitop:syntax:a {1}
        seed_roaring_bitmap bitmap:roaring:bitop:syntax:b {2}

        assert_error {ERR syntax error} {
            r bitop noop bitmap:roaring:bitop:syntax:dest bitmap:roaring:bitop:syntax:a bitmap:roaring:bitop:syntax:b
        }
        assert_error {ERR BITOP NOT*} {
            r bitop not bitmap:roaring:bitop:syntax:dest bitmap:roaring:bitop:syntax:a bitmap:roaring:bitop:syntax:b
        }
        assert_error {ERR BITOP DIFF*} {
            r bitop diff bitmap:roaring:bitop:syntax:dest bitmap:roaring:bitop:syntax:a
        }
        assert_error {ERR BITOP DIFF1*} {
            r bitop diff1 bitmap:roaring:bitop:syntax:dest bitmap:roaring:bitop:syntax:a
        }
        assert_error {ERR BITOP ANDOR*} {
            r bitop andor bitmap:roaring:bitop:syntax:dest bitmap:roaring:bitop:syntax:a
        }
    }

    test {BITOP handles Roaring bitmap empty sources and destination aliasing} {
        seed_roaring_bitmap bitmap:roaring:bitop:empty {}
        assert_equal 0 [r bitop not bitmap:roaring:bitop:empty-not bitmap:roaring:bitop:empty]
        assert_equal 0 [r exists bitmap:roaring:bitop:empty-not]

        seed_string_bitmap bitmap:roaring:bitop:alias:string:dest {0 2 4 6}
        seed_string_bitmap bitmap:roaring:bitop:alias:string:other {2 6 8}
        seed_roaring_bitmap bitmap:roaring:bitop:alias:roaring:dest {0 2 4 6}
        seed_roaring_bitmap bitmap:roaring:bitop:alias:roaring:other {2 6 8}

        set string_reply [r bitop diff bitmap:roaring:bitop:alias:string:dest bitmap:roaring:bitop:alias:string:dest bitmap:roaring:bitop:alias:string:other]
        set roaring_reply [r bitop diff bitmap:roaring:bitop:alias:roaring:dest bitmap:roaring:bitop:alias:roaring:dest bitmap:roaring:bitop:alias:roaring:other]
        assert_equal $string_reply $roaring_reply
        assert_equal [r get bitmap:roaring:bitop:alias:string:dest] [r debug bitmap-raw bitmap:roaring:bitop:alias:roaring:dest]
        assert_equal bitmap [r type bitmap:roaring:bitop:alias:roaring:dest]
    }

    test {BITOP frees an aliased Roaring destination without touching stale sources} {
        # Regression: the destination's old value is also a source here, and
        # both store branches dispose of it (the delete branch frees it
        # outright), so bitopCommand()'s cleanup loop must not dereference
        # the source objects afterwards.

        # Delete branch: all-empty Roaring sources with an aliased destination.
        seed_roaring_bitmap bitmap:roaring:bitop:self:empty {}
        assert_equal 0 [r bitop and bitmap:roaring:bitop:self:empty bitmap:roaring:bitop:self:empty]
        assert_equal 0 [r exists bitmap:roaring:bitop:self:empty]

        # Store branch: a self-targeting OR keeps the same bits.
        seed_roaring_bitmap bitmap:roaring:bitop:self:or {0 3 70000}
        assert_equal 8751 [r bitop or bitmap:roaring:bitop:self:or bitmap:roaring:bitop:self:or]
        assert_equal bitmap [r type bitmap:roaring:bitop:self:or]
        assert_equal 3 [r bitcount bitmap:roaring:bitop:self:or]
        assert_equal {1 1 1} [list \
            [r getbit bitmap:roaring:bitop:self:or 0] \
            [r getbit bitmap:roaring:bitop:self:or 3] \
            [r getbit bitmap:roaring:bitop:self:or 70000]]
    }

    test {BITOP mixed roaring and string sources match string results for all operations} {
        set a [binary format H* f000ff]
        set b [binary format H* 0f0f]
        set c [binary format H* 33000080]
        set raws [list $a $b $c]

        foreach op {and or xor diff diff1 andor one} {
            assert_roaring_bitop_raws_match_string "mixed:$op" $op $raws {0 2}
        }
        assert_roaring_bitop_raws_match_string mixed:not not [list $a] {0}
    }

    test {BITOP mixed dense string chunks match string results} {
        set roaring [binary format H* [string repeat aa 8194]]
        set dense [binary format H* "[string repeat ff 8192]8001"]
        set sparse [binary format H* [string repeat 00 8194]]
        set sparse [string replace $sparse 0 0 [binary format H* 80]]
        set sparse [string replace $sparse 8191 8191 [binary format H* 01]]
        set sparse [string replace $sparse 8192 8192 [binary format H* 80]]
        set sparse [string replace $sparse 8193 8193 [binary format H* 01]]
        set raws [list $roaring $dense $sparse]

        foreach op {and or xor diff diff1 andor one} {
            assert_roaring_bitop_raws_match_string "mixed-dense:$op" \
                $op $raws {0}
        }
    }

    test {BITOP mixed benchmark-shaped dense operands match string results} {
        set a [binary format H* [string repeat 2f 4096]]
        set b [binary format H* [string repeat 9a 4096]]
        set c [binary format H* [string repeat f1 4096]]
        set d [binary format H* [string repeat 6d 4096]]
        set raws [list $a $b $c $d]

        foreach op {and or xor diff diff1 andor one} {
            assert_roaring_bitop_raws_match_string "mixed-benchmark:$op" \
                $op $raws {1 3}
        }
    }

    test {BITOP mixed AVX512-shaped operands and scalar tails match string results} {
        set raws {}
        set patterns {2f 9a f1 6d 87 3c d2 55}
        for {set i 0} {$i < 8} {incr i} {
            # Eight sources with minlen >= 10000 select the AVX512 kernel when
            # available. Unequal lengths ending off the 64-byte boundary also
            # exercise the portable zero-padded tail on every platform.
            lappend raws [binary format H* [string repeat \
                [lindex $patterns $i] [expr {10000 + $i}]]]
        }

        foreach op {and or xor diff diff1 andor one} {
            assert_roaring_bitop_raws_match_string "mixed-avx512:$op" \
                $op $raws {0 2 4 6}
        }
    }

    test {BITOP mixed roaring source destination aliasing matches string results} {
        set a [binary format H* aa5500]
        set b [binary format H* 0ff0]
        set c [binary format H* 330000f0]
        set raws [list $a $b $c]

        foreach {op alias_index roaring_indexes} {
            and   0 {0 2}
            or    1 {1 2}
            xor   2 {0 2}
            diff  0 {0 2}
            diff1 1 {1 2}
            andor 2 {0 2}
            one   0 {0 2}
        } {
            assert_roaring_bitop_raws_match_string "alias:$op:$alias_index" \
                $op $raws $roaring_indexes $alias_index
        }

        foreach {op alias_index roaring_indexes} {
            and   0 {2}
            or    1 {0 2}
            xor   2 {0}
            diff  0 {2}
            diff1 1 {0 2}
            andor 2 {0}
            one   0 {2}
        } {
            assert_roaring_bitop_raws_match_string "alias-string:$op:$alias_index" \
                $op $raws $roaring_indexes $alias_index
        }

        assert_roaring_bitop_raws_match_string alias:not not [list $a] {0} 0
    }

    test {BITOP mixed roaring fuzz matches bitmap-default-roaring no strings} {
        foreach op {and or xor diff diff1 andor one} {
            set min_args 1
            if {$op eq "diff" || $op eq "diff1" || $op eq "andor"} {
                set min_args 2
            }

            for {set i 0} {$i < 12} {incr i} {
                set raws {}
                set roaring_indexes {}
                set count [expr {$min_args + [randomInt 4]}]

                for {set j 0} {$j < $count} {incr j} {
                    lappend raws [randstring 0 128]
                    if {[expr {($i + $j) % 2}] == 0} {
                        lappend roaring_indexes $j
                    }
                }

                assert_roaring_bitop_raws_match_string "fuzz:$op:$i" \
                    $op $raws $roaring_indexes
            }
        }

        for {set i 0} {$i < 12} {incr i} {
            assert_roaring_bitop_raws_match_string "fuzz:not:$i" \
                not [list [randstring 0 128]] {0}
        }
    }

    test {BITOP mixed roaring and missing-key sources match string results} {
        r config set bitmap-default-roaring no

        set a [binary format H* f0f0]
        set c [binary format H* 0f]

        foreach op {and or xor diff diff1 andor one} {
            r del bitop:miss:string:dest bitop:miss:roaring:dest
            r del bitop:miss:string:a bitop:miss:string:gone bitop:miss:string:c
            r del bitop:miss:roaring:a bitop:miss:roaring:gone bitop:miss:roaring:c

            r set bitop:miss:string:a $a
            r set bitop:miss:string:c $c
            r set bitop:miss:roaring:a $a
            r set bitop:miss:roaring:c $c
            convert_string_bitmap_to_roaring r bitop:miss:roaring:a
            set string_reply [r bitop $op bitop:miss:string:dest \
                bitop:miss:string:a bitop:miss:string:gone bitop:miss:string:c]
            set roaring_reply [r bitop $op bitop:miss:roaring:dest \
                bitop:miss:roaring:a bitop:miss:roaring:gone bitop:miss:roaring:c]
            assert_equal $string_reply $roaring_reply
            assert_equal [bitmap_logical_raw bitop:miss:string:dest] \
                [bitmap_logical_raw bitop:miss:roaring:dest]
        }
    }

    test {BITOP with a missing first source matches string results on the Roaring path} {
        # The empty-accumulator seeding branches (sources[0] == NULL) are
        # distinct code paths: AND/ANDOR clear the result, DIFF1 skips the
        # andnot, and the generic copy falls back to an empty roaring.
        r config set bitmap-default-roaring no

        set a [binary format H* f0f0]
        set c [binary format H* 0f]

        foreach op {and or xor diff diff1 andor one} {
            r del bitop:first:string:dest bitop:first:roaring:dest
            r del bitop:first:string:gone bitop:first:string:a bitop:first:string:c
            r del bitop:first:roaring:gone bitop:first:roaring:a bitop:first:roaring:c

            r set bitop:first:string:a $a
            r set bitop:first:string:c $c
            r set bitop:first:roaring:a $a
            r set bitop:first:roaring:c $c
            convert_string_bitmap_to_roaring r bitop:first:roaring:a
            set string_reply [r bitop $op bitop:first:string:dest \
                bitop:first:string:gone bitop:first:string:a bitop:first:string:c]
            set roaring_reply [r bitop $op bitop:first:roaring:dest \
                bitop:first:roaring:gone bitop:first:roaring:a bitop:first:roaring:c]
            assert_equal $string_reply $roaring_reply
            assert_equal [bitmap_logical_raw bitop:first:string:dest] \
                [bitmap_logical_raw bitop:first:roaring:dest]
        }
    }

    test {BITOP duplicate sources match string results on the Roaring path} {
        r config set bitmap-default-roaring no

        set a [binary format H* aa5500]
        set s [binary format H* 0ff0]

        # The same Roaring bitmap key twice: both slots borrow the same
        # roaring, so the accumulator must deep-copy rather than steal.
        foreach op {and or xor diff diff1 andor one} {
            r del bitop:dup:string:dest bitop:dup:roaring:dest
            r del bitop:dup:string:k bitop:dup:roaring:k
            r set bitop:dup:string:k $a
            r set bitop:dup:roaring:k $a
            convert_string_bitmap_to_roaring r bitop:dup:roaring:k
            set string_reply [r bitop $op bitop:dup:string:dest \
                bitop:dup:string:k bitop:dup:string:k]
            set roaring_reply [r bitop $op bitop:dup:roaring:dest \
                bitop:dup:roaring:k bitop:dup:roaring:k]
            assert_equal $string_reply $roaring_reply
            assert_equal [bitmap_logical_raw bitop:dup:string:dest] \
                [bitmap_logical_raw bitop:dup:roaring:dest]
        }

        # The same string key twice alongside a roaring source. These small
        # sources take the mixed raw-word path; the next test covers repeated
        # string sources on the Roaring path.
        foreach op {and or xor diff diff1 andor one} {
            r del bitop:dup2:string:dest bitop:dup2:roaring:dest
            r del bitop:dup2:string:s bitop:dup2:roaring:s
            r del bitop:dup2:string:n bitop:dup2:roaring:n
            r set bitop:dup2:string:s $s
            r set bitop:dup2:roaring:s $s
            r set bitop:dup2:string:n $a
            r set bitop:dup2:roaring:n $a
            convert_string_bitmap_to_roaring r bitop:dup2:roaring:n
            set string_reply [r bitop $op bitop:dup2:string:dest \
                bitop:dup2:string:s bitop:dup2:string:s bitop:dup2:string:n]
            set roaring_reply [r bitop $op bitop:dup2:roaring:dest \
                bitop:dup2:roaring:s bitop:dup2:roaring:s bitop:dup2:roaring:n]
            assert_equal $string_reply $roaring_reply
            assert_equal [bitmap_logical_raw bitop:dup2:string:dest] \
                [bitmap_logical_raw bitop:dup2:roaring:dest]
        }
    }

    test {BITOP Roaring path reads repeated and interleaved string sources} {
        # String sources are converted one at a time and a key read again
        # reuses the live conversion. Interleave repeats with other strings,
        # a native source and a missing key, including DIFF1/ANDOR reading the
        # first source after the union, so a stale conversion would show.
        # The native source is sparse, which keeps the mixed raw-word path
        # out of the way; patterns without it use bitmap-default-roaring yes.
        r config set bitmap-default-roaring no

        set a [binary format H* f0f00f0f55]
        set b [binary format H* 0ff0aa]

        r del bitop:lazy:string:gone bitop:lazy:roaring:gone
        foreach side {string roaring} {
            r set bitop:lazy:$side:a $a
            r set bitop:lazy:$side:b $b
        }
        seed_string_bitmap bitop:lazy:string:n {100}
        seed_roaring_bitmap bitop:lazy:roaring:n {100}

        foreach op {and or xor diff diff1 andor one} {
            foreach pattern {
                {a a a}
                {a b a b a}
                {b a a gone a b}
                {n a a b a n}
                {a n a b n b}
            } {
                set string_sources {}
                set roaring_sources {}
                foreach k $pattern {
                    lappend string_sources bitop:lazy:string:$k
                    lappend roaring_sources bitop:lazy:roaring:$k
                }
                set roaring_default [expr {[lsearch -exact $pattern n] >= 0 ? "no" : "yes"}]

                r del bitop:lazy:string:dest bitop:lazy:roaring:dest
                set string_reply [r bitop $op bitop:lazy:string:dest {*}$string_sources]
                r config set bitmap-default-roaring $roaring_default
                set roaring_reply [r bitop $op bitop:lazy:roaring:dest {*}$roaring_sources]
                r config set bitmap-default-roaring no

                assert_equal $string_reply $roaring_reply
                assert_equal string [r type bitop:lazy:string:dest]
                assert_equal bitmap [r type bitop:lazy:roaring:dest]
                assert_equal [bitmap_logical_raw bitop:lazy:string:dest] \
                    [bitmap_logical_raw bitop:lazy:roaring:dest]
            }
        }
    }

    test {BITOP rejects non-string non-bitmap sources mixed with Roaring bitmaps} {
        seed_roaring_bitmap bitop:wrongtype:roaring {0 9}
        r del bitop:wrongtype:list bitop:wrongtype:dest
        r rpush bitop:wrongtype:list element

        # The type error fires after earlier sources may already be prepared,
        # exercising the cleanup of converted operands under sanitizer runs.
        assert_error {WRONGTYPE*} {
            r bitop and bitop:wrongtype:dest bitop:wrongtype:roaring bitop:wrongtype:list
        }
        assert_error {WRONGTYPE*} {
            r bitop xor bitop:wrongtype:dest bitop:wrongtype:list bitop:wrongtype:roaring
        }
        assert_equal 0 [r exists bitop:wrongtype:dest]
        assert_equal bitmap [r type bitop:wrongtype:roaring]
        assert_equal bitmap-roaring [r object encoding bitop:wrongtype:roaring]
    }
}

# used_memory_peak is never reset, not even by CONFIG RESETSTAT, so the peak
# is measured on a server of its own.
start_server {tags {"bitmap" "bitmap-roaring" "external:skip" "cluster:skip"}} {
    test {Roaring BITOP peak memory does not grow with the number of string sources} {
        # Longer than the mixed raw-word path's 1 MiB result cap, so a native
        # source takes the Roaring path too. The dense all-ones string is
        # built in the server to keep the query buffer small.
        set len [expr {2 * 1024 * 1024}]
        set strings [lrepeat 16 bitop:peak:s]
        r config set bitmap-default-roaring no
        r setbit bitop:peak:zeros [expr {$len * 8 - 1}] 0
        r bitop not bitop:peak:s bitop:peak:zeros
        r del bitop:peak:zeros
        seed_roaring_bitmap bitop:peak:native {0}
        # Distinct keys with the same bytes are separate objects, so each one
        # is converted after the previous conversion is freed.
        set distinct {}
        for {set i 0} {$i < 8} {incr i} {
            r copy bitop:peak:s bitop:peak:s$i
            lappend distinct bitop:peak:s$i
        }

        # At most the accumulator (the union for DIFF1/ANDOR) and one string
        # conversion are alive at once; ONE also keeps the bits seen more than
        # once and a per-source intersection. Converting every argument up
        # front grew the peak by one copy per argument. ONE runs last so its
        # higher peak is not charged to the other operations.
        foreach {op bound} {and 3 or 3 xor 3 diff 3 diff1 3 andor 3 one 6} {
            # Config-selected Roaring over strings only, then a native source
            # with the config off, each with one key repeated and with
            # distinct keys.
            foreach {roaring_default native} {yes {} no bitop:peak:native} {
                foreach {kind sources} [list repeated $strings distinct $distinct] {
                    r config set bitmap-default-roaring $roaring_default
                    r del bitop:peak:dest
                    set before [s used_memory]
                    assert_equal $len [r bitop $op bitop:peak:dest {*}$native {*}$sources]
                    set growth [expr {[s used_memory_peak] - $before}]
                    assert_equal bitmap [r type bitop:peak:dest]
                    assert_lessthan $growth [expr {$bound * $len}] \
                        "BITOP $op over $kind sources with bitmap-default-roaring $roaring_default"
                }
            }
        }
        r config set bitmap-default-roaring no
    }
}

# Randomized differential fuzz over multi-container values. A chunk is the
# 8192-byte slice of a raw value that becomes one 2^16-bit Roaring container,
# and its shape selects the container type after conversion: an all-zero chunk
# leaves no container, sparse chunks become arrays, chunks with 4095 to 4097
# set bits sit on either side of the array container limit, random bytes need
# a bitset, and long runs of ones or zeroes become run containers.
proc bitroar_fuzz_chunk {len} {
    switch [randomInt 7] {
        0 {return [string repeat \x00 $len]}
        1 {return [string repeat \xff $len]}
        2 {
            set words {}
            for {set i 0} {$i < ($len + 3) / 4} {incr i} {
                lappend words [randomInt 4294967296]
            }
            return [string range [binary format I* $words] 0 [expr {$len - 1}]]
        }
        3 {
            set bytes [lrepeat $len 0]
            for {set i [randomInt 128]} {$i >= 0} {incr i -1} {
                set byte [randomInt $len]
                lset bytes $byte [expr {[lindex $bytes $byte] | (1 << [randomInt 8])}]
            }
        }
        4 {
            set bytes [lrepeat $len 0]
            set count [expr {min($len, 4095 + [randomInt 3])}]
            for {set i 0} {$i < $count} {incr i} {
                lset bytes $i [expr {1 << [randomInt 8]}]
            }
        }
        default {
            # Runs of ones over zeroes, or of zeroes over ones, with random
            # edge bytes so most runs do not start or end on a byte boundary.
            set fill [expr {[randomInt 2] ? 0 : 255}]
            set bytes [lrepeat $len $fill]
            for {set runs [randomInt 4]} {$runs >= 0} {incr runs -1} {
                set start [randomInt $len]
                set end [expr {min($len, $start + 1 + [randomInt [expr {$len / 4 + 1}]])}]
                for {set i $start} {$i < $end} {incr i} {
                    lset bytes $i [expr {255 - $fill}]
                }
                if {$start > 0} {lset bytes [expr {$start - 1}] [randomInt 256]}
                if {$end < $len} {lset bytes $end [randomInt 256]}
            }
        }
    }
    return [binary format c* $bytes]
}

# Random raw value of $min_chunks to $max_chunks chunks. The last chunk is
# partial half of the time, so the logical length does not always end on a
# container boundary.
proc bitroar_fuzz_raw {min_chunks max_chunks} {
    set chunks [expr {$min_chunks + [randomInt [expr {$max_chunks - $min_chunks + 1}]]}]
    set raw {}
    for {set i 1} {$i <= $chunks} {incr i} {
        set len 8192
        if {$i == $chunks && [randomInt 2]} {
            set len [expr {1 + [randomInt 8192]}]
        }
        append raw [bitroar_fuzz_chunk $len]
    }
    return $raw
}

# Bits where container walks start and stop: the first and last set bit and
# the first and last clear bit of every chunk of $raw.
proc bitroar_fuzz_edges {raw} {
    binary scan $raw B* bits
    set size [string length $bits]
    set edges {}
    for {set base 0} {$base < $size} {incr base 65536} {
        set last [expr {min($base + 65535, $size - 1)}]
        foreach bit {0 1} {
            set first [string first $bit $bits $base]
            if {$first >= 0 && $first <= $last} {
                lappend edges $first [string last $bit $bits $last]
            }
        }
    }
    return $edges
}

# Random position in a value of $units bytes or bits, $chunk units per
# container: anywhere in the value, next to a container boundary or to one of
# the $edges of the value, or past the end.
proc bitroar_fuzz_position {units chunk edges} {
    randpath {
        randomInt [expr {$units + 1}]
    } {
        expr {[randomInt [expr {$units / $chunk + 2}]] * $chunk + [randomInt 3] - 1}
    } {
        expr {[lindex $edges [randomInt [llength $edges]]] + [randomInt 3] - 1}
    } {
        expr {$units + [randomInt $chunk]}
    }
}

# Random BITCOUNT/BITPOS start and end over $units bytes or bits. Either the
# two positions are independent, so start > end is common, or one of them is
# close to the other, so short ranges inside a single gap or run of ones that
# stop right at a container boundary or edge are common too. A third of the
# time each index is counted from the end, which can also land before the
# start of the value.
proc bitroar_fuzz_range {units chunk edges} {
    set start [bitroar_fuzz_position $units $chunk $edges]
    set span [randomInt [expr {[randomInt 2] ? 16 : $chunk}]]
    switch [randomInt 3] {
        0 {set end [bitroar_fuzz_position $units $chunk $edges]}
        1 {set end [expr {$start + $span}]}
        2 {
            set end $start
            set start [expr {$end - $span}]
        }
    }
    set range {}
    foreach index [list $start $end] {
        if {[randomInt 3] == 0} {incr index [expr {-$units}]}
        lappend range $index
    }
    return $range
}

# Random BITFIELD type and offset for a value of $bits bits: a plain offset up
# to one field past the end or straddling a container boundary, or #N.
proc bitroar_fuzz_field {bits} {
    if {[randomInt 2]} {
        set width [expr {1 + [randomInt 64]}]
        set type i$width
    } else {
        # u64 is not supported by BITFIELD.
        set width [expr {1 + [randomInt 63]}]
        set type u$width
    }
    set offset [randpath {
        randomInt [expr {$bits + 64}]
    } {
        expr {max(0, [randomInt [expr {$bits / 65536 + 2}]] * 65536 - [randomInt $width])}
    } {
        format #%d [randomInt [expr {$bits / $width + 2}]]
    }]
    return [list $type $offset]
}

# Random BITFIELD SET value or INCRBY increment for a $width-bit field: small,
# within twice the field range so overflows are common, or an int64 extreme.
proc bitroar_fuzz_value {width} {
    randpath {
        expr {[randomInt 512] - 256}
    } {
        set value [expr {entier(rand() * 2.0 ** ($width + 1)) - 2 ** $width}]
        expr {max(-9223372036854775808, min(9223372036854775807, $value))}
    } {
        lindex {-9223372036854775808 -1 0 1 9223372036854775807} [randomInt 5]
    }
}

# Random BITCOUNT, BITPOS or BITFIELD_RO command for a value of $len bytes
# with the given bit $edges, with "key" standing for the key name.
proc bitroar_fuzz_read_command {len edges} {
    set bits [expr {$len * 8}]
    set byte_edges [lmap edge $edges {expr {$edge / 8}}]
    set byte_range [bitroar_fuzz_range $len 8192 $byte_edges]
    set bit_range [concat [bitroar_fuzz_range $bits 65536 $edges] bit]
    switch [randomInt 3] {
        0 {
            set cmd [list bitcount key]
            switch [randomInt 5] {
                0 {}
                1 {lappend cmd {*}$byte_range}
                2 {lappend cmd {*}$byte_range byte}
                default {lappend cmd {*}$bit_range}
            }
        }
        1 {
            set cmd [list bitpos key [randomInt 2]]
            switch [randomInt 6] {
                0 {}
                1 {lappend cmd [lindex $byte_range 0]}
                2 {lappend cmd {*}$byte_range}
                3 {lappend cmd {*}$byte_range byte}
                default {lappend cmd {*}$bit_range}
            }
        }
        default {
            set cmd [list bitfield_ro key]
            for {set i [randomInt 3]} {$i >= 0} {incr i -1} {
                lappend cmd get {*}[bitroar_fuzz_field $bits]
            }
        }
    }
    return $cmd
}

# Random BITFIELD command for a value of $len bytes mixing GET, SET and INCRBY
# under every OVERFLOW mode, with "key" standing for the key name.
proc bitroar_fuzz_write_command {len} {
    set cmd [list bitfield key]
    for {set i [randomInt 3]} {$i >= 0} {incr i -1} {
        if {[randomInt 3] == 0} {
            lappend cmd overflow [lindex {wrap sat fail} [randomInt 3]]
        }
        lassign [bitroar_fuzz_field [expr {$len * 8}]] type offset
        set width [string range $type 1 end]
        switch [randomInt 5] {
            0 {lappend cmd get $type $offset}
            1 - 2 {lappend cmd set $type $offset [bitroar_fuzz_value $width]}
            default {lappend cmd incrby $type $offset [bitroar_fuzz_value $width]}
        }
    }
    return $cmd
}

# Reply of $command run against $key, or its error, so that error replies are
# compared as well.
proc bitroar_fuzz_reply {key command} {
    if {[catch {r {*}[lreplace $command 1 1 $key]} reply]} {
        return "error: $reply"
    }
    return $reply
}

# Compare possibly large logical values, reporting the first differing byte
# instead of dumping both.
proc assert_bitroar_fuzz_raw_equal {value expected detail} {
    if {$value eq $expected} return
    set i 0
    while {[string index $value $i] eq [string index $expected $i]} {
        incr i
    }
    fail "Bitmap values differ at byte $i (lengths [string length $value]\
        and [string length $expected]) $detail"
}

start_server {tags {"bitmap" "bitmap-roaring" "needs:debug" "cluster:skip"}} {
    # A SET larger than the default 4096-byte channel buffer leaves the client
    # in several writes, and Nagle's algorithm then stalls it for a delayed
    # ACK. Send each of these values of up to 48KB in a single write.
    fconfigure [r channel] -buffersize 65536

    test {Multi-container Roaring BITCOUNT BITPOS and BITFIELD fuzz matches strings} {
        # Replace the seed with the one a failure reports to reproduce it.
        set seed [clock milliseconds]
        expr {srand($seed)}
        if {$::verbose} {puts "Multi-container Roaring fuzz seed: $seed"}
        if {$::accurate} {set bitmaps 120} else {set bitmaps 24}

        r config set bitmap-default-roaring no
        for {set i 0} {$i < $bitmaps} {incr i} {
            set raw [bitroar_fuzz_raw 3 6]
            r set bitmap:fuzz:string $raw
            r set bitmap:fuzz:roaring $raw
            convert_string_bitmap_to_roaring r bitmap:fuzz:roaring
            set len [string length $raw]
            set edges [bitroar_fuzz_edges $raw]

            # The first reads see the containers built by the conversion, the
            # rest interleave with BITFIELD writes that reshape and grow them.
            for {set j 0} {$j < 300} {incr j} {
                if {$j < 150 || [randomInt 2]} {
                    set cmd [bitroar_fuzz_read_command $len $edges]
                } else {
                    set cmd [bitroar_fuzz_write_command $len]
                }
                assert_equal [bitroar_fuzz_reply bitmap:fuzz:string $cmd] \
                    [bitroar_fuzz_reply bitmap:fuzz:roaring $cmd] \
                    "(seed $seed, bitmap $i, command $j: $cmd)"
                if {[lindex $cmd 0] eq "bitfield"} {
                    set len [r strlen bitmap:fuzz:string]
                }
            }
            set detail "(seed $seed, bitmap $i)"
            assert_bitroar_fuzz_raw_equal [r debug bitmap-raw bitmap:fuzz:roaring] \
                [r get bitmap:fuzz:string] $detail
            assert_equal bitmap [r type bitmap:fuzz:roaring] $detail
            assert_equal bitmap-roaring [r object encoding bitmap:fuzz:roaring] $detail
        }
    }

    test {BITOP multi-container Roaring fuzz matches bitmap-default-roaring no strings} {
        # Replace the seed with the one a failure reports to reproduce it.
        set seed [clock milliseconds]
        expr {srand($seed)}
        if {$::verbose} {puts "Multi-container Roaring BITOP fuzz seed: $seed"}
        if {$::accurate} {set iterations 40} else {set iterations 8}

        r config set bitmap-default-roaring no
        foreach op {and or xor diff diff1 andor one not} {
            for {set i 0} {$i < $iterations} {incr i} {
                if {$op eq "not"} {
                    set count 1
                } elseif {$op in {diff diff1 andor}} {
                    set count [expr {2 + [randomInt 3]}]
                } else {
                    set count [expr {1 + [randomInt 4]}]
                }

                # Each source is missing, or is a string on one side and a
                # string or a Roaring bitmap on the other, with lengths from
                # one to six chunks. The destination may alias a source.
                set string_sources {}
                set roaring_sources {}
                set raws {}
                set has_roaring 0
                for {set j 0} {$j < $count} {incr j} {
                    set string_key bitmap:fuzz:bitop:string:$j
                    set roaring_key bitmap:fuzz:bitop:roaring:$j
                    lappend string_sources $string_key
                    lappend roaring_sources $roaring_key
                    if {[randomInt 8] == 0} {
                        r del $string_key $roaring_key
                        lappend raws {}
                        continue
                    }
                    set raw [bitroar_fuzz_raw 1 6]
                    r set $string_key $raw
                    r set $roaring_key $raw
                    if {[randomInt 2]} {
                        convert_string_bitmap_to_roaring r $roaring_key
                        set has_roaring 1
                    }
                    lappend raws $raw
                }
                set alias -1
                set string_dest bitmap:fuzz:bitop:string:dest
                set roaring_dest bitmap:fuzz:bitop:roaring:dest
                if {[randomInt 4] == 0} {
                    set alias [randomInt $count]
                    set string_dest [lindex $string_sources $alias]
                    set roaring_dest [lindex $roaring_sources $alias]
                }

                set detail "(seed $seed, op $op, iteration $i)"
                assert_equal [r bitop $op $string_dest {*}$string_sources] \
                    [r bitop $op $roaring_dest {*}$roaring_sources] $detail
                assert_equal [r exists $string_dest] [r exists $roaring_dest] $detail
                assert_bitroar_fuzz_raw_equal [bitmap_logical_raw $roaring_dest] \
                    [bitmap_logical_raw $string_dest] $detail
                if {[r exists $roaring_dest]} {
                    # At least one Roaring source makes the destination Roaring.
                    assert_equal string [r type $string_dest] $detail
                    assert_equal [expr {$has_roaring ? "bitmap" : "string"}] \
                        [r type $roaring_dest] $detail

                    # BITOP builds the destination containers itself instead of
                    # converting bytes, so compare reads over them as well.
                    set dest_raw [r get $string_dest]
                    set edges [bitroar_fuzz_edges $dest_raw]
                    for {set j 0} {$j < 20} {incr j} {
                        set cmd [bitroar_fuzz_read_command [string length $dest_raw] $edges]
                        assert_equal [bitroar_fuzz_reply $string_dest $cmd] \
                            [bitroar_fuzz_reply $roaring_dest $cmd] \
                            "(seed $seed, op $op, iteration $i, command $j: $cmd)"
                    }
                }
                for {set j 0} {$j < $count} {incr j} {
                    if {$j == $alias} continue
                    assert_bitroar_fuzz_raw_equal \
                        [bitmap_logical_raw [lindex $roaring_sources $j]] \
                        [lindex $raws $j] $detail
                    assert_bitroar_fuzz_raw_equal \
                        [bitmap_logical_raw [lindex $string_sources $j]] \
                        [lindex $raws $j] $detail
                }
            }
        }
    }
}


start_server {tags {"bitmap" "bitmap-roaring" "cluster:skip"}} {
    test {Roaring bitmap BITOP supports OLAP columnar index user stories} {
        # Inspired by Apache Druid's columnar segment and logical filter docs:
        # https://druid.apache.org/docs/latest/design/segments/
        # https://druid.apache.org/docs/latest/querying/filters/#logical-expression-filters
        #
        # Rows model ad-tech events in a Druid-style columnar segment:
        # 0 {country Brazil clicks 1 gender male}
        # 1 {country United States impressions 1 gender female}
        # 2 {country Brazil clicks 1 gender female}
        # 3 {country United States clicks 1 gender male}
        # 4 {country United States installs 1 gender female}
        # 5 {country United States impressions 1 gender female}
        # 6 {country Israel impressions 1 gender male}
        # 7 {country United States installs 1 gender female}
        #
        # Each dimension or metric value is indexed by the row IDs that match it.
        foreach {index bits} {
            country:brazil {0 2}
            country:united-states {1 3 4 5 7}
            country:israel {6}
            gender:male {0 3 6}
            gender:female {1 2 4 5 7}
            metric:clicks {0 2 3}
            metric:impressions {1 5 6}
            metric:installs {4 7}
            universe {0 1 2 3 4 5 6 7}
        } {
            seed_roaring_bitmap "bitmap:olap:$index" $bits
            assert_equal bitmap [r type "bitmap:olap:$index"]
            assert_equal bitmap-roaring [r object encoding "bitmap:olap:$index"]
        }

        # Query: how many Brazil users installed the app?
        r bitop and bitmap:olap:q:brazil-installs \
            bitmap:olap:country:brazil bitmap:olap:metric:installs
        assert_bitmap_has_exact_bits bitmap:olap:q:brazil-installs {}

        # Query: how many female users clicked but did not install?
        #
        # The NOT predicate must be bounded by the segment universe. Otherwise
        # complementing a bitmap may include bits outside the ingested rows.
        # All eight rows fill one byte. Pad the logical length with a zero in
        # the next byte so NOT still exercises a bit outside the universe.
        r setbit bitmap:olap:metric:installs 8 0
        r bitop not bitmap:olap:q:not-installs:raw bitmap:olap:metric:installs
        assert_equal 1 [r getbit bitmap:olap:q:not-installs:raw 8]

        r bitop and bitmap:olap:q:not-installs \
            bitmap:olap:universe bitmap:olap:q:not-installs:raw
        assert_bitmap_has_exact_bits bitmap:olap:q:not-installs {0 1 2 3 5 6}
        assert_equal 0 [r getbit bitmap:olap:q:not-installs 8]

        r bitop and bitmap:olap:q:female-click-no-install \
            bitmap:olap:gender:female bitmap:olap:metric:clicks bitmap:olap:q:not-installs
        assert_bitmap_has_exact_bits bitmap:olap:q:female-click-no-install {2}
        assert_equal bitmap [r type bitmap:olap:q:female-click-no-install]

        # Query: how many United States users clicked or saw an impression?
        r bitop or bitmap:olap:q:engaged \
            bitmap:olap:metric:clicks bitmap:olap:metric:impressions
        assert_bitmap_has_exact_bits bitmap:olap:q:engaged {0 1 2 3 5 6}

        r bitop and bitmap:olap:q:us-engaged \
            bitmap:olap:country:united-states bitmap:olap:q:engaged
        assert_bitmap_has_exact_bits bitmap:olap:q:us-engaged {1 3 5}
        assert_equal bitmap [r type bitmap:olap:q:us-engaged]
    }

    test {Roaring bitmap BITOP models Pinot inverted index examples} {
        # Implements the Apache Pinot Star-Tree Index example table and inverted
        # index story as Redis bitmaps over document IDs:
        # https://docs.pinot.apache.org/build-with-pinot/indexing/star-tree-index
        #
        # 0 {Country CA  Browser Chrome  Locale en  Impressions 400}
        # 1 {Country CA  Browser Firefox Locale fr  Impressions 200}
        # 2 {Country MX  Browser Safari  Locale es  Impressions 300}
        # 3 {Country MX  Browser Safari  Locale en  Impressions 100}
        # 4 {Country USA Browser Chrome  Locale en  Impressions 600}
        # 5 {Country USA Browser Firefox Locale es  Impressions 200}
        # 6 {Country USA Browser Firefox Locale en  Impressions 400}
        foreach {index bits} {
            country:ca {0 1}
            country:mx {2 3}
            country:usa {4 5 6}
            browser:chrome {0 4}
            browser:firefox {1 5 6}
            browser:safari {2 3}
            locale:en {0 3 4 6}
            locale:fr {1}
            locale:es {2 5}
            metric:impressions-at-least-400 {0 4 6}
            universe {0 1 2 3 4 5 6}
        } {
            seed_roaring_bitmap "bitmap:pinot:$index" $bits
            assert_equal bitmap [r type "bitmap:pinot:$index"]
            assert_equal bitmap-roaring [r object encoding "bitmap:pinot:$index"]
        }

        # Source story: an inverted index maps a value such as Browser=Firefox
        # to the matching document IDs.
        assert_bitmap_has_exact_bits bitmap:pinot:browser:firefox {1 5 6}
        assert_bitmap_has_exact_bits bitmap:pinot:locale:en {0 3 4 6}

        # Query: which Firefox documents are in the English locale?
        r bitop and bitmap:pinot:q:firefox-en \
            bitmap:pinot:browser:firefox bitmap:pinot:locale:en
        assert_bitmap_has_exact_bits bitmap:pinot:q:firefox-en {6}

        # Query: which USA documents used Chrome or Spanish locale?
        r bitop or bitmap:pinot:q:chrome-or-es \
            bitmap:pinot:browser:chrome bitmap:pinot:locale:es
        assert_bitmap_has_exact_bits bitmap:pinot:q:chrome-or-es {0 2 4 5}

        r bitop and bitmap:pinot:q:usa-chrome-or-es \
            bitmap:pinot:country:usa bitmap:pinot:q:chrome-or-es
        assert_bitmap_has_exact_bits bitmap:pinot:q:usa-chrome-or-es {4 5}

        # Query: which USA documents have at least 400 impressions?
        r bitop and bitmap:pinot:q:usa-high-impressions \
            bitmap:pinot:country:usa bitmap:pinot:metric:impressions-at-least-400
        assert_bitmap_has_exact_bits bitmap:pinot:q:usa-high-impressions {4 6}

        # Query: which CA or MX documents are not in the French locale?
        r bitop or bitmap:pinot:q:ca-or-mx \
            bitmap:pinot:country:ca bitmap:pinot:country:mx
        r bitop not bitmap:pinot:q:not-fr:raw bitmap:pinot:locale:fr
        assert_equal 1 [r getbit bitmap:pinot:q:not-fr:raw 7]

        r bitop and bitmap:pinot:q:not-fr \
            bitmap:pinot:universe bitmap:pinot:q:not-fr:raw
        r bitop and bitmap:pinot:q:ca-or-mx-not-fr \
            bitmap:pinot:q:ca-or-mx bitmap:pinot:q:not-fr
        assert_bitmap_has_exact_bits bitmap:pinot:q:ca-or-mx-not-fr {0 2 3}
    }
}


start_server {tags {"bitmap" "bitmap-roaring" "cluster:skip"}} {
    test {Roaring bitmap BITOP models Druid Wikipedia query tutorial filters} {
        # Implements the Apache Druid query tutorial's Wikipedia-style OLAP
        # story as Redis bitmaps over row IDs:
        # https://druid.apache.org/docs/latest/tutorials/tutorial-query/
        #
        # 0 {page Copa America countryName United States channel en isRobot false}
        # 1 {page Copa America countryName Brazil        channel es isRobot false}
        # 2 {page Lionel Messi countryName Argentina    channel es isRobot false}
        # 3 {page Apache Druid countryName null         channel en isRobot true}
        # 4 {page Apache Druid countryName United States channel en isRobot false}
        # 5 {page Wind countryName null                 channel de isRobot false}
        # 6 {page Copa America countryName United States channel en isRobot true}
        foreach {index bits} {
            page:copa-america {0 1 6}
            page:apache-druid {3 4}
            page:lionel-messi {2}
            page:wind {5}
            country:united-states {0 4 6}
            country:brazil {1}
            country:argentina {2}
            country:null {3 5}
            channel:en {0 3 4 6}
            channel:es {1 2}
            channel:de {5}
            isrobot:true {3 6}
            isrobot:false {0 1 2 4 5}
            universe {0 1 2 3 4 5 6}
        } {
            seed_roaring_bitmap "bitmap:wikipedia:$index" $bits
            assert_equal bitmap [r type "bitmap:wikipedia:$index"]
            assert_equal bitmap-roaring [r object encoding "bitmap:wikipedia:$index"]
        }

        # Tutorial query pattern: exclude rows without a countryName value.
        r bitop not bitmap:wikipedia:q:country-not-null:raw bitmap:wikipedia:country:null
        assert_equal 1 [r getbit bitmap:wikipedia:q:country-not-null:raw 7]

        r bitop and bitmap:wikipedia:q:country-not-null \
            bitmap:wikipedia:universe bitmap:wikipedia:q:country-not-null:raw
        assert_bitmap_has_exact_bits \
            bitmap:wikipedia:q:country-not-null {0 1 2 4 6}
        assert_equal 0 [r getbit bitmap:wikipedia:q:country-not-null 7]

        # Tutorial query pattern: group by page and countryName, then count rows.
        r bitop and bitmap:wikipedia:q:copa-country-edits \
            bitmap:wikipedia:page:copa-america bitmap:wikipedia:q:country-not-null
        assert_bitmap_has_exact_bits \
            bitmap:wikipedia:q:copa-country-edits {0 1 6}
        assert_equal 3 [r bitcount bitmap:wikipedia:q:copa-country-edits]

        r bitop and bitmap:wikipedia:q:copa-us-edits \
            bitmap:wikipedia:page:copa-america bitmap:wikipedia:country:united-states
        assert_bitmap_has_exact_bits \
            bitmap:wikipedia:q:copa-us-edits {0 6}
        assert_equal 2 [r bitcount bitmap:wikipedia:q:copa-us-edits]

        # Query: English edits with a countryName value, excluding robot edits.
        r bitop not bitmap:wikipedia:q:not-robot:raw bitmap:wikipedia:isrobot:true
        r bitop and bitmap:wikipedia:q:not-robot \
            bitmap:wikipedia:universe bitmap:wikipedia:q:not-robot:raw
        r bitop and bitmap:wikipedia:q:en-country-not-robot \
            bitmap:wikipedia:channel:en \
            bitmap:wikipedia:q:country-not-null \
            bitmap:wikipedia:q:not-robot
        assert_bitmap_has_exact_bits \
            bitmap:wikipedia:q:en-country-not-robot {0 4}

        # Query: edits from either Brazil or Argentina.
        r bitop or bitmap:wikipedia:q:brazil-or-argentina \
            bitmap:wikipedia:country:brazil bitmap:wikipedia:country:argentina
        assert_bitmap_has_exact_bits \
            bitmap:wikipedia:q:brazil-or-argentina {1 2}
    }
}

start_server {tags {"bitmap" "bitmap-roaring" "needs:debug" "needs:save" "cluster:skip"}} {
    test {Roaring bitmap RDB save and reload survive lowering proto-max-bulk-len} {
        r config set bitmap-default-roaring yes
        r del bitmap:proto:shrink
        r setbit bitmap:proto:shrink 16777215 1
        r config set bitmap-default-roaring no
        assert_equal bitmap [r type bitmap:proto:shrink]

        # Persistence is independent of the current client protocol limit.
        set old [config_get_set proto-max-bulk-len 1048576]
        r debug reload
        assert_equal bitmap [r type bitmap:proto:shrink]
        assert_equal bitmap-roaring [r object encoding bitmap:proto:shrink]
        assert_equal 1 [r bitcount bitmap:proto:shrink]

        r config set proto-max-bulk-len $old
        assert_equal 1 [r getbit bitmap:proto:shrink 16777215]
        r del bitmap:proto:shrink
    }

    if {[s arch_bits] == 64} {
        test {Roaring bitmap RDB reload stays compact at a 2^40 bit offset} {
            set high_bit [expr {(1 << 40) - 1}]
            set byte_len [expr {($high_bit >> 3) + 1}]
            set old_limit [config_get_set proto-max-bulk-len $byte_len]

            r config set bitmap-default-roaring yes
            r del bitmap:rdb:reload-high
            r setbit bitmap:rdb:reload-high $high_bit 1
            r config set bitmap-default-roaring no
            r config set proto-max-bulk-len 1048576

            r debug reload
            assert_equal bitmap [r type bitmap:rdb:reload-high]
            assert_lessthan [r memory usage bitmap:rdb:reload-high] 65536

            r config set proto-max-bulk-len $byte_len
            assert_equal 1 [r getbit bitmap:rdb:reload-high $high_bit]
            r del bitmap:rdb:reload-high
            r config set proto-max-bulk-len $old_limit
        }
    }

    test {Roaring bitmap RDB round-trip with rdbcompression no} {
        set old [config_get_set rdbcompression no]
        r config set bitmap-default-roaring yes
        r del bitmap:nocompress
        foreach bit {0 5 64 1000 65536 100000} {
            r setbit bitmap:nocompress $bit 1
        }
        r config set bitmap-default-roaring no

        set digest [debug_digest_value bitmap:nocompress]
        r debug reload
        assert_equal bitmap [r type bitmap:nocompress]
        assert_equal $digest [debug_digest_value bitmap:nocompress]
        assert_equal 6 [r bitcount bitmap:nocompress]

        r config set rdbcompression $old
        r del bitmap:nocompress
    }

    test {redis-check-rdb validates dumps containing Roaring bitmaps} {
        r config set bitmap-default-roaring yes
        r del bitmap:checkrdb:sparse bitmap:checkrdb:dense
        r setbit bitmap:checkrdb:sparse 9 1
        r setbit bitmap:checkrdb:sparse 100000 1
        r setrange bitmap:checkrdb:dense 0 [string repeat "\xff" 4096]
        r setbit bitmap:checkrdb:dense 200000 1 ;# converts the dense string
        r config set bitmap-default-roaring no
        assert_equal bitmap [r type bitmap:checkrdb:sparse]
        assert_equal bitmap [r type bitmap:checkrdb:dense]

        r save
        set dump_path [file join [lindex [r config get dir] 1] dump.rdb]
        set res [exec src/redis-check-rdb $dump_path]
        assert_match "*RDB looks OK*" $res

        r del bitmap:checkrdb:sparse bitmap:checkrdb:dense
    }

    test {redis-check-rdb reports a truncated Roaring bitmap as an unexpected EOF} {
        r flushall
        create_roaring_bitmap_from_bits r bitmap:checkrdb:truncated \
            {3 70000 140000 210000 280000 1000000}
        r save
        set dir [lindex [r config get dir] 1]
        set fd [open [file join $dir dump.rdb] rb]
        set rdb [read $fd]
        close $fd

        # The bitmap is the only key, so it is the last value before the EOF
        # opcode and the 8-byte checksum. Cutting the file inside its portable
        # blob is a short read, which must not be reported as corruption: a
        # replica loading from a socket resumes after read errors but exits on
        # corruption errors.
        set truncated_path [file join $dir truncated.rdb]
        set fd [open $truncated_path wb]
        puts -nonewline $fd [string range $rdb 0 end-14]
        close $fd
        catch {exec src/redis-check-rdb $truncated_path} res
        file delete $truncated_path
        assert_match "*Unexpected EOF reading RDB file*" $res
        assert_no_match "*Invalid bitmap RDB payload*" $res
        r del bitmap:checkrdb:truncated
    }
}

run_solo {bitroar-large-memory} {
start_server {tags {"bitmap" "bitmap-roaring" "cluster:skip"}} {
    test {BITOP NOT missing-chunk budget does not apply to string sources} {
        set allocation_envelope [expr {65536 * (1 << 13)}]
        set byte_len [expr {$allocation_envelope + 1}]
        r config set proto-max-bulk-len [expr {$byte_len + 16}]
        r config set bitmap-default-roaring yes
        r del bitop:not:string:src bitop:not:string:dest

        # The source itself already occupies memory proportional to the work,
        # so the Roaring missing-chunk amplification budget does not apply.
        assert_equal $byte_len [r setrange bitop:not:string:src \
            [expr {$byte_len - 1}] [binary format H* 80]]
        assert_equal string [r type bitop:not:string:src]

        assert_equal $byte_len [r bitop not bitop:not:string:dest \
            bitop:not:string:src]
        assert_equal bitmap [r type bitop:not:string:dest]
        assert_equal 1 [r getbit bitop:not:string:dest 0]
        assert_equal 0 [r getbit bitop:not:string:dest \
            [expr {($byte_len - 1) * 8}]]
        assert_equal [expr {$byte_len * 8 - 1}] \
            [r bitcount bitop:not:string:dest]

        r del bitop:not:string:src bitop:not:string:dest
        set _ {}
    } {} {large-memory config:restore}
}
} ;# run_solo
