source tests/support/bitmap.tcl

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

    test {BITCONVERT is internal and has narrow conversion semantics} {
        set raw [binary format H* 80400100080000]
        r del bitmap_missing bitmap_string bitmap_list

        # Ordinary clients cannot discover these primitives through COMMAND or execute them.
        assert_equal {{}} [r command info bitconvert]
        assert_equal {{}} [r command info bitop_roaring]
        assert_error {ERR unknown command 'bitconvert'*} {
            r bitconvert bitmap_missing ROARING
        }

        r debug mark-internal-client

        # A missing key becomes an empty native bitmap.
        assert_equal OK [r bitconvert bitmap_missing ROARING]
        assert_equal bitmap [r type bitmap_missing]
        assert_equal bitmap-roaring [r object encoding bitmap_missing]
        assert_equal {} [r debug bitmap-raw bitmap_missing]

        # Strings convert in place without changing bytes or expiration.
        r set bitmap_string $raw
        r pexpire bitmap_string 600000
        set expire_at [r pexpiretime bitmap_string]
        assert_equal OK [r bitconvert bitmap_string ROARING]
        assert_equal bitmap [r type bitmap_string]
        assert_equal $raw [r debug bitmap-raw bitmap_string]
        assert_equal $expire_at [r pexpiretime bitmap_string]

        # Converting a native bitmap again is an idempotent no-op.
        set digest [r debug digest-value bitmap_string]
        assert_equal OK [r bitconvert bitmap_string ROARING]
        assert_equal $digest [r debug digest-value bitmap_string]
        assert_equal $expire_at [r pexpiretime bitmap_string]

        # Other value types retain their ordinary WRONGTYPE behavior.
        r lpush bitmap_list value
        assert_error {WRONGTYPE*} {r bitconvert bitmap_list ROARING}
        assert_equal list [r type bitmap_list]

        assert_error {ERR syntax error*} {
            r bitconvert bitmap_missing STRING
        }

        # Config-independent BITOP replay uses a second hidden primitive that
        # performs the native store and notifications in one operation.
        r set bitmap_source [binary format H* f0]
        assert_equal 1 [r bitop_roaring or bitmap_out bitmap_source]
        assert_equal bitmap [r type bitmap_out]
        assert_equal [binary format H* f0] \
            [r debug bitmap-raw bitmap_out]

        r debug mark-internal-client unmark
        assert_error {ERR unknown command 'bitconvert'*} {
            r bitconvert bitmap_missing ROARING
        }
    }

    test {Internal bitmap propagation primitives cannot be renamed} {
        foreach command {bitconvert bitop_roaring} {
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
            {0fdbe68d29365ad0882498117659e397bf5050ba}
            bitmap:digest:ranges:multi {1 2 3 4}
            {bfafd3f1520764ed3706f6448ee778884e151323}
            bitmap:digest:ranges:cross-container {65534 65535 65536 65537}
            {92210d5971fd016b830e89475c07b04104263d7d}
        } {
            assert_equal OK [create_roaring_bitmap_from_bits r $key $bits]
            assert_equal bitmap-roaring [r object encoding $key]
            assert_equal $expected_digest [r debug digest-value $key]
        }
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
        r config set bitmap-default-roaring yes

        r del bitmap:public:watch
        r set bitmap:public:watch ""
        r watch bitmap:public:watch
        assert_equal 0 [r setbit bitmap:public:watch $sparse_public_offset 1]
        assert_equal bitmap [r type bitmap:public:watch]
        r multi
        r ping
        assert_equal {} [r exec]
        r config set bitmap-default-roaring no
    }

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
        $rd psubscribe __keyevent@9__:*
        $rd read

        # Direct roaring creation in bitmap-default-roaring yes: same event
        # names as a legacy creating SETBIT ("new" then "setbit"), with the
        # write event classified under the bitmap notification class.
        r config set bitmap-default-roaring yes
        r setbit bitmap:public:notify $sparse_public_offset 1
        assert_equal {pmessage __keyevent@9__:* __keyevent@9__:new bitmap:public:notify} [$rd read]
        assert_equal {pmessage __keyevent@9__:* __keyevent@9__:setbit bitmap:public:notify} [$rd read]

        # A no-op SETBIT still converts the representation. BITCONVERT emits
        # type_changed; SETBIT then observes the native value and has no
        # logical write event, exactly as it does during replay.
        assert_equal 1 [r setbit bitmap:public:notify:conv 0 1]
        assert_equal {pmessage __keyevent@9__:* __keyevent@9__:type_changed bitmap:public:notify:conv} [$rd read]

        # BITFIELD follows the same replay-equivalent contract when its write
        # leaves the logical bits unchanged.
        assert_equal {1} [r bitfield bitmap:public:notify:bitfield SET u8 0 1]
        assert_equal {pmessage __keyevent@9__:* __keyevent@9__:type_changed bitmap:public:notify:bitfield} [$rd read]

        # The representation transition still occurs when every write is
        # rejected by OVERFLOW FAIL, but the rejected command emits no event.
        assert_equal {{}} [r bitfield bitmap:public:notify:bitfield:fail \
            OVERFLOW FAIL INCRBY u8 0 1]
        assert_equal bitmap [r type bitmap:public:notify:bitfield:fail]
        assert_equal {pmessage __keyevent@9__:* __keyevent@9__:type_changed bitmap:public:notify:bitfield:fail} [$rd read]
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
        $rd psubscribe __keyevent@9__:*
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
        assert_equal {pmessage __keyevent@9__:* __keyevent@9__:setbit bitmap:notify:string-dollar} [$rd read]

        r config set notify-keyspace-events Eb
        r setbit bitmap:notify:string-bitmap 0 1
        r config set bitmap-default-roaring yes
        r setbit bitmap:notify:roaring-bitmap 0 1
        assert_equal {pmessage __keyevent@9__:* __keyevent@9__:setbit bitmap:notify:roaring-bitmap} [$rd read]
        assert_equal 1 [r bitop or bitmap:notify:bitop-bitmap \
            bitmap:notify:bitop-source]
        assert_equal {pmessage __keyevent@9__:* __keyevent@9__:set bitmap:notify:bitop-bitmap} [$rd read]

        r config set notify-keyspace-events EA
        r setbit bitmap:notify:roaring-all 0 1
        assert_equal {pmessage __keyevent@9__:* __keyevent@9__:setbit bitmap:notify:roaring-all} [$rd read]

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

    test {BITOP destination always uses Roaring with bitmap-default-roaring yes} {
        r config set bitmap-default-roaring no
        r del bitop:imp:s1 bitop:imp:s2 bitop:imp:out
        r set bitop:imp:s1 [binary format H* cc]
        r set bitop:imp:s2 [binary format H* aa]

        r config set bitmap-default-roaring yes
        assert_equal 1 [r bitop xor bitop:imp:out bitop:imp:s1 bitop:imp:s2]
        assert_equal bitmap [r type bitop:imp:out]
        assert_equal [binary format H* 66] [r debug bitmap-raw bitop:imp:out]
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

        # A valid portable bitmap must consume the entire RDB string payload.
        set trailing [binary format H* "21011f${one_bit}001000${checksum}"]
        assert_error {*Bad data format*} {
            r restore bitmap:rdb:trailing 0 $trailing
        }
        assert_equal 0 [r exists bitmap:rdb:invalid-len bitmap:rdb:trailing]
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

    test {Roaring bitmap unlink uses lazyfree for many roaring containers} {
        r config resetstat
        r config set bitmap-default-roaring yes
        for {set i 0} {$i < 80} {incr i} {
            r setbit bitmap:lazy [expr {$i * 65536}] 1
        }
        r config set bitmap-default-roaring no
        assert_equal [r type bitmap:lazy] bitmap

        assert_equal [r unlink bitmap:lazy] 1
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

        # A config-driven BITOP propagates a force-native internal form so its
        # store and notification sequence is identical during replay.
        r set bitmap:aof-incr:bitop:s1 [binary format H* f0]
        r set bitmap:aof-incr:bitop:s2 [binary format H* 0f]
        r config set bitmap-default-roaring yes
        assert_equal 1 [r bitop or bitmap:aof-incr:bitop:out \
            bitmap:aof-incr:bitop:s1 bitmap:aof-incr:bitop:s2]
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
            if {$name in {multi exec bitconvert setbit bitfield bitop bitop_roaring}} {
                lappend transitions $cmd
            }
            if {$name eq "restore" && [string match "bitmap:aof-incr:*" [lindex $cmd 1]]} {
                incr transition_restores
            }
        }
        close $fp

        assert_equal [list \
            {multi} \
            [list bitconvert bitmap:aof-incr:create ROARING] \
            [list setbit bitmap:aof-incr:create $sparse_public_offset 1] \
            {exec} \
            {multi} \
            [list bitconvert bitmap:aof-incr:convert ROARING] \
            [list setbit bitmap:aof-incr:convert 0 1] \
            {exec} \
            {multi} \
            [list bitconvert bitmap:aof-incr:bitfield ROARING] \
            [list bitfield bitmap:aof-incr:bitfield SET u1 0 1] \
            {exec} \
            [list bitop_roaring or bitmap:aof-incr:bitop:out \
                bitmap:aof-incr:bitop:s1 bitmap:aof-incr:bitop:s2]] $transitions
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
        }

        test {BITOP destinations replicate deterministically across modes} {
            # String-only sources with a bitmap-default-roaring yes master: the
            # destination decision is master-local, so the stream carries the
            # force-native BITOP_ROARING primitive.
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

            foreach {mode constant on_replica in_aof} {
                none REPL_NONE 0 0
                aof REPL_AOF 0 1
                replica REPL_REPLICA 1 0
                all REPL_ALL 1 1
            } {
                foreach command {setbit bitfield bitop} {
                    set key bitmap:selective:$mode:$command
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
                    assert_equal bitmap [$master type $key]
                    lappend cases $key $on_replica $in_aof
                }
            }

            wait_for_ofs_sync $master $replica
            foreach {key on_replica in_aof} $cases {
                if {$on_replica} {
                    assert_equal bitmap [$replica type $key]
                } else {
                    assert_equal none [$replica type $key]
                }
            }

            # Detach before replacing the master's live dataset from its AOF;
            # only REPL_AOF and REPL_ALL transition pairs should be present.
            $replica replicaof no one
            $master debug loadaof
            foreach {key on_replica in_aof} $cases {
                if {$in_aof} {
                    assert_equal bitmap [$master type $key]
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

        set equivalent_keys [list $source $copy $restored]
        set before [r memory usage $source]
        assert_morethan $before 0
        foreach key $equivalent_keys {
            assert_equal bitmap-roaring [r object encoding $key]
            assert_equal 3 [r bitcount $key]
            assert_equal $before [r memory usage $key] \
                "key=$key before_mutation"
        }
        set bitop_before [r memory usage $bitop]
        assert_morethan $bitop_before 0
        assert_equal $bitop_before [r memory usage $bitop_reference]

        foreach key [concat $equivalent_keys $bitop $bitop_reference] {
            assert_equal 0 [r setbit $key 196608 1]
            assert_equal 4 [r bitcount $key]
        }
        set after [r memory usage $source]
        assert_morethan $after $before
        foreach key $equivalent_keys {
            assert_equal $after [r memory usage $key] \
                "key=$key after_mutation"
        }
        set bitop_after [r memory usage $bitop]
        assert_morethan $bitop_after $bitop_before
        assert_equal $bitop_after [r memory usage $bitop_reference]

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

    test {BITOP NOT bounds work for compact high-length Roaring bitmaps} {
        set not_limit [expr {512 * 1024 * 1024}]
        set byte_len [expr {$not_limit + 1}]
        set last_bit [expr {$byte_len * 8 - 1}]
        r config set proto-max-bulk-len $byte_len
        r config set bitmap-default-roaring yes
        r del bitop:not:roaring:huge bitop:not:roaring:huge:dest \
            bitop:not:roaring:huge:copy bitop:not:roaring:limit \
            bitop:not:roaring:limit:dest

        # The exact limit, whose flip endpoint is 2^32 bits, remains valid.
        set limit_last_bit [expr {$not_limit * 8 - 1}]
        assert_equal 0 [r setbit bitop:not:roaring:limit $limit_last_bit 0]
        assert_equal $not_limit [r bitop not bitop:not:roaring:limit:dest \
            bitop:not:roaring:limit]
        assert_equal 1 [r getbit bitop:not:roaring:limit:dest 0]
        assert_equal 1 [r getbit bitop:not:roaring:limit:dest $limit_last_bit]
        r del bitop:not:roaring:limit bitop:not:roaring:limit:dest

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
        assert_error {ERR BITOP NOT result exceeds 512 MiB Roaring bitmap limit} {
            r bitop not bitop:not:roaring:huge:dest bitop:not:roaring:huge
        }
        assert_equal keep [r get bitop:not:roaring:huge:dest]
        assert_equal $dirty [s rdb_changes_since_last_save]

        # Aliasing is rejected before the source can be replaced. Other BITOPs
        # retain wide sparse support and preserve the compact logical length.
        assert_error {ERR BITOP NOT result exceeds 512 MiB Roaring bitmap limit} {
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

        # The same string key twice alongside a roaring source: each slot
        # builds an independent owned roaring, so the slot-0 steal cannot
        # affect the second operand.
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


start_server {tags {"bitmap" "bitmap-roaring" "cluster:skip"}} {
    test {Roaring bitmap BITOP supports OLAP columnar index user stories} {
        # Inspired by Apache Druid's columnar segment and logical filter docs:
        # https://druid.apache.org/docs/latest/design/segments/
        # https://druid.apache.org/docs/latest/querying/filters/#logical-expression-filters
        #
        # Rows model ad-tech events in a Druid-style columnar segment:
        # 0 {country Brazil clicks 1 gender male}
        # 1 {country Brazil clicks 1 gender female}
        # 2 {country United States impressions 1 gender female}
        # 3 {country United States clicks 1 gender male}
        # 4 {country United States installs 1 gender female}
        # 5 {country United States impressions 1 gender female}
        # 6 {country United States installs 1 gender female}
        #
        # Each dimension or metric value is indexed by the row IDs that match it.
        foreach {index bits} {
            country:brazil {0 1}
            country:united-states {2 3 4 5 6}
            gender:male {0 3}
            gender:female {1 2 4 5 6}
            metric:clicks {0 1 3}
            metric:impressions {2 5}
            metric:installs {4 6}
            universe {0 1 2 3 4 5 6}
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
        r bitop not bitmap:olap:q:not-installs:raw bitmap:olap:metric:installs
        assert_equal 1 [r getbit bitmap:olap:q:not-installs:raw 7]

        r bitop and bitmap:olap:q:not-installs \
            bitmap:olap:universe bitmap:olap:q:not-installs:raw
        assert_bitmap_has_exact_bits bitmap:olap:q:not-installs {0 1 2 3 5}
        assert_equal 0 [r getbit bitmap:olap:q:not-installs 7]

        r bitop and bitmap:olap:q:female-click-no-install \
            bitmap:olap:gender:female bitmap:olap:metric:clicks bitmap:olap:q:not-installs
        assert_bitmap_has_exact_bits bitmap:olap:q:female-click-no-install {1}
        assert_equal bitmap [r type bitmap:olap:q:female-click-no-install]

        # Query: how many United States users clicked or saw an impression?
        r bitop or bitmap:olap:q:engaged \
            bitmap:olap:metric:clicks bitmap:olap:metric:impressions
        assert_bitmap_has_exact_bits bitmap:olap:q:engaged {0 1 2 3 5}

        r bitop and bitmap:olap:q:us-engaged \
            bitmap:olap:country:united-states bitmap:olap:q:engaged
        assert_bitmap_has_exact_bits bitmap:olap:q:us-engaged {2 3 5}
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
}
