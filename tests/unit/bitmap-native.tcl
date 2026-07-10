source tests/support/bitmap.tcl

set testmodule [file normalize tests/modules/misc.so]
set ::sparse_public_offset 65536
set ::sparse_public_len 8193

start_server {tags {"bitmap" "bitmap-native" "needs:debug" "cluster:skip"}} {
    test {bitmap-default-roaring defaults to no} {
        assert_equal no [lindex [r config get bitmap-default-roaring] 1]
    }

    test {BITMAP command is not part of the v1 public surface} {
        assert_equal {{}} [r command info bitmap]
        assert_equal {{}} [r command info bitmap|convert]
        assert_error {ERR unknown command 'bitmap'*} {r bitmap help}
        assert_error {ERR unknown command 'bitmap'*} {r bitmap convert bitmap:convert:missing}
    }

    test {bitmap-default-roaring no: SETBIT keeps creating strings} {
        r config set bitmap-default-roaring no

        assert_equal 0 [r setbit bitmap:public:disabled $::sparse_public_offset 1]
        assert_equal string [r type bitmap:public:disabled]
        assert_equal $::sparse_public_len [r strlen bitmap:public:disabled]
        assert_equal 1 [r getbit bitmap:public:disabled $::sparse_public_offset]
    }

    test {bitmap-default-roaring yes: SETBIT creates native bitmaps for missing keys} {
        r config set bitmap-default-roaring yes

        assert_equal 0 [r setbit bitmap:public:create $::sparse_public_offset 1]
        assert_equal bitmap [r type bitmap:public:create]
        assert_equal bitmap-roaring [r object encoding bitmap:public:create]
        assert_equal 1 [r getbit bitmap:public:create $::sparse_public_offset]
        assert_equal 1 [r bitcount bitmap:public:create]
        assert_equal $::sparse_public_len [string length [r debug bitmap-raw bitmap:public:create]]
        assert_error {WRONGTYPE*} {r get bitmap:public:create}
        r config set bitmap-default-roaring no
    }

    test {bitmap-default-roaring yes: SETBIT converts existing string values and keeps TTL} {
        r config set bitmap-default-roaring yes

        r set bitmap:public:auto-on ""
        r pexpire bitmap:public:auto-on 60000
        assert_equal 0 [r setbit bitmap:public:auto-on $::sparse_public_offset 1]
        assert_equal bitmap [r type bitmap:public:auto-on]
        assert_equal bitmap-roaring [r object encoding bitmap:public:auto-on]
        assert_equal 1 [r getbit bitmap:public:auto-on $::sparse_public_offset]
        assert {[r pttl bitmap:public:auto-on] > 0}
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

    test {bitmap-default-roaring yes: zero SETBIT extends native bitmap length} {
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

    test {bitmap-default-roaring yes: BITFIELD creates and converts native bitmaps} {
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

    test {bitmap-default-roaring converts non-empty strings to native bitmaps and keeps TTL} {
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

    test {native bitmap dump restore preserves all-zero logical byte length} {
        r config set bitmap-default-roaring no
        set raw [string repeat [binary format H* 00] 6]

        r del bitmap:convert:zeros bitmap:convert:zeros:restored
        r set bitmap:convert:zeros $raw
        assert_equal OK [convert_string_bitmap_to_native r bitmap:convert:zeros]
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

    test {empty native bitmap fixtures preserve zero logical byte length} {
        r del bitmap:fixture:empty
        assert_equal OK [create_native_bitmap_from_raw r bitmap:fixture:empty ""]
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
        assert_equal OK [convert_string_bitmap_to_native r bitmap:convert:int]
        assert_equal bitmap [r type bitmap:convert:int]
        assert_equal "12345" [r debug bitmap-raw bitmap:convert:int]

        r config set bitmap-default-roaring yes
        r rpush bitmap:convert:list element
        assert_error {WRONGTYPE*} {r setbit bitmap:convert:list 0 1}
        r config set bitmap-default-roaring no
    }

    test {DEBUG DIGEST for native bitmaps includes trailing zero length} {
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

    test {DEBUG DIGEST for native bitmaps ignores roaring container encoding} {
        r del bitmap:digest:converted bitmap:digest:setbit
        set raw [binary format H* [string repeat ff 1024]]

        r set bitmap:digest:converted $raw
        assert_equal OK [convert_string_bitmap_to_native r bitmap:digest:converted]

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

    test {native bitmap writes keep the proto-max-bulk-len offset limit} {
        r del bitmap:native:bounds
        r config set bitmap-default-roaring yes
        assert_equal 0 [r setbit bitmap:native:bounds 0 1]
        r config set bitmap-default-roaring no

        assert_equal bitmap [r type bitmap:native:bounds]
        assert_equal 1 [r bitcount bitmap:native:bounds]
        assert_equal 0 [r getbit bitmap:native:bounds 4294967295]
        assert_equal {0} [r bitfield_ro bitmap:native:bounds GET u1 4294967295]
        foreach cmd {
            {getbit bitmap:native:bounds 4294967296}
            {setbit bitmap:native:bounds 4294967296 1}
            {bitfield bitmap:native:bounds SET u1 4294967296 1}
            {bitfield_ro bitmap:native:bounds GET u1 4294967296}
            {bitfield bitmap:native:bounds GET u1 4294967296 SET u1 0 1}
        } {
            assert_error {*bit offset*out of range*} {r {*}$cmd}
        }
        assert_error {*bit offset*out of range*} {
            r setbit bitmap:native:bounds 9223372036854775808 1
        }
        assert_equal 1 [r bitcount bitmap:native:bounds]
        r del bitmap:native:bounds
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

        r del bitmap:native:small-limit
        r config set bitmap-default-roaring yes
        assert_equal 0 [r setbit bitmap:native:small-limit $last_allowed 1]
        assert_equal 1 [r getbit bitmap:native:small-limit $last_allowed]

        foreach cmd [list \
            [list getbit bitmap:native:small-limit $first_rejected] \
            [list bitfield_ro bitmap:native:small-limit GET u1 $first_rejected] \
            [list setbit bitmap:native:small-limit $first_rejected 1] \
            [list bitfield bitmap:native:small-limit SET u1 $first_rejected 1] \
            [list bitfield bitmap:native:small-limit GET u1 $first_rejected SET u1 0 0] \
        ] {
            assert_error {*bit offset*out of range*} {r {*}$cmd}
        }
        # Like string bitmaps, a BITFIELD write whose offset passes the limit
        # may span up to 63 bits past it.
        assert_equal {2} [r bitfield bitmap:native:small-limit SET u2 $last_allowed 3]
        assert_equal 2 [r bitcount bitmap:native:small-limit]

        r config set bitmap-default-roaring no
        r config set proto-max-bulk-len $oldval
        r del bitmap:native:small-limit
    }

    test {native bitmap offset cap remains bounded when proto-max-bulk-len is raised} {
        set raised_limit [expr {536870912 + 1}]
        set max_native_bit 4294967295
        set first_rejected [expr {$max_native_bit + 1}]
        set oldval [config_get_set proto-max-bulk-len $raised_limit]

        r del bitmap:native:raised-limit bitmap:native:raised-limit:new \
            bitmap:native:raised-limit:string
        r config set bitmap-default-roaring yes
        assert_equal 0 [r setbit bitmap:native:raised-limit 0 1]
        assert_equal 0 [r getbit bitmap:native:raised-limit $max_native_bit]

        # Writes past the fixed native cap fail even though proto-max-bulk-len
        # would permit the offset; reads there see plain unset bits, exactly
        # like reads past the end of a string bitmap.
        foreach cmd [list \
            [list setbit bitmap:native:raised-limit $first_rejected 1] \
            [list bitfield bitmap:native:raised-limit SET u1 $first_rejected 1] \
            [list bitfield bitmap:native:raised-limit SET u2 $max_native_bit 3] \
        ] {
            assert_error {*bit offset*out of range*} {r {*}$cmd}
        }
        assert_equal 0 [r getbit bitmap:native:raised-limit $first_rejected]
        assert_equal {0} [r bitfield_ro bitmap:native:raised-limit GET u1 $first_rejected]
        assert_equal {0 1} [
            r bitfield bitmap:native:raised-limit GET u1 $first_rejected SET u1 0 1
        ]

        assert_error {*bit offset*out of range*} {
            r setbit bitmap:native:raised-limit:new $first_rejected 1
        }
        assert_equal 0 [r exists bitmap:native:raised-limit:new]

        # A write past the native cap against an existing string is rejected
        # by the conversion path and must leave the string untouched: SETBIT
        # discards the trial native object, BITFIELD rejects before
        # converting. Reads there pass the raised parse-time limit and see
        # zeros past the end of the string.
        r set bitmap:native:raised-limit:string [binary format H* 80]
        assert_error {*bit offset*out of range*} {
            r setbit bitmap:native:raised-limit:string $first_rejected 1
        }
        assert_error {*bit offset*out of range*} {
            r bitfield bitmap:native:raised-limit:string SET u1 $first_rejected 1
        }
        assert_equal {0} [
            r bitfield bitmap:native:raised-limit:string GET u1 $first_rejected
        ]
        assert_equal string [r type bitmap:native:raised-limit:string]
        assert_equal [binary format H* 80] [r get bitmap:native:raised-limit:string]

        assert_equal 1 [r bitcount bitmap:native:raised-limit]
        r config set bitmap-default-roaring no
        r config set proto-max-bulk-len $oldval
        r del bitmap:native:raised-limit bitmap:native:raised-limit:string
    }

    test {WATCH aborts the transaction when bitmap-default-roaring converts the key} {
        r config set bitmap-default-roaring yes

        r del bitmap:public:watch
        r set bitmap:public:watch ""
        r watch bitmap:public:watch
        assert_equal 0 [r setbit bitmap:public:watch $::sparse_public_offset 1]
        assert_equal bitmap [r type bitmap:public:watch]
        r multi
        r ping
        assert_equal {} [r exec]
        r config set bitmap-default-roaring no
    }

    test {native bitmap creation and conversion emit documented keyspace events} {
        r config set bitmap-default-roaring no
        r del bitmap:public:notify bitmap:public:notify:conv

        r config set notify-keyspace-events E\$ocnb
        set rd [redis_deferring_client]
        $rd psubscribe __keyevent@9__:*
        $rd read

        # Direct native creation in bitmap-default-roaring yes: same event
        # names as a legacy creating SETBIT ("new" then "setbit"), with the
        # write event classified under the bitmap notification class.
        r config set bitmap-default-roaring yes
        r setbit bitmap:public:notify $::sparse_public_offset 1
        assert_equal {pmessage __keyevent@9__:* __keyevent@9__:new bitmap:public:notify} [$rd read]
        assert_equal {pmessage __keyevent@9__:* __keyevent@9__:setbit bitmap:public:notify} [$rd read]

        # bitmap-default-roaring conversion: the converting SETBIT additionally emits the
        # overwrite pair from setKey() before the trailing "setbit", so for
        # subscribers and modules the conversion is an observable type
        # change rather than a silent in-place mutation.
        r config set bitmap-default-roaring no
        r set bitmap:public:notify:conv ""
        assert_equal {pmessage __keyevent@9__:* __keyevent@9__:new bitmap:public:notify:conv} [$rd read]
        assert_equal {pmessage __keyevent@9__:* __keyevent@9__:set bitmap:public:notify:conv} [$rd read]
        r config set bitmap-default-roaring yes
        r setbit bitmap:public:notify:conv $::sparse_public_offset 1
        assert_equal {pmessage __keyevent@9__:* __keyevent@9__:overwritten bitmap:public:notify:conv} [$rd read]
        assert_equal {pmessage __keyevent@9__:* __keyevent@9__:type_changed bitmap:public:notify:conv} [$rd read]
        assert_equal {pmessage __keyevent@9__:* __keyevent@9__:setbit bitmap:public:notify:conv} [$rd read]
        r config set bitmap-default-roaring no

        $rd close
        r config set notify-keyspace-events {}
    }

    test {native bitmap writes use only the bitmap notification class} {
        r config set bitmap-default-roaring no
        r del bitmap:notify:native-dollar bitmap:notify:string-dollar \
            bitmap:notify:string-bitmap bitmap:notify:native-bitmap \
            bitmap:notify:native-all

        set rd [redis_deferring_client]
        $rd psubscribe __keyevent@9__:*
        $rd read

        r config set notify-keyspace-events E\$
        r config set bitmap-default-roaring yes
        r setbit bitmap:notify:native-dollar 0 1
        r config set bitmap-default-roaring no
        r setbit bitmap:notify:string-dollar 0 1
        assert_equal {pmessage __keyevent@9__:* __keyevent@9__:setbit bitmap:notify:string-dollar} [$rd read]

        r config set notify-keyspace-events Eb
        r setbit bitmap:notify:string-bitmap 0 1
        r config set bitmap-default-roaring yes
        r setbit bitmap:notify:native-bitmap 0 1
        assert_equal {pmessage __keyevent@9__:* __keyevent@9__:setbit bitmap:notify:native-bitmap} [$rd read]

        r config set notify-keyspace-events EA
        r setbit bitmap:notify:native-all 0 1
        assert_equal {pmessage __keyevent@9__:* __keyevent@9__:setbit bitmap:notify:native-all} [$rd read]

        $rd close
        r config set bitmap-default-roaring no
        r config set notify-keyspace-events {}
    }

    test {public native bitmaps cover the bitmap command surface} {
        r config set bitmap-default-roaring yes

        assert_equal 0 [r setbit bitmap:public:commands $::sparse_public_offset 1]
        assert_equal 1 [r getbit bitmap:public:commands $::sparse_public_offset]
        assert_equal 1 [r bitcount bitmap:public:commands]
        assert_equal $::sparse_public_offset [r bitpos bitmap:public:commands 1]
        assert_equal [list 1] [r bitfield_ro bitmap:public:commands GET u1 $::sparse_public_offset]

        set next_offset [expr {$::sparse_public_offset + 1}]
        assert_equal [list 0] [r bitfield bitmap:public:commands SET u1 $next_offset 1]
        assert_equal bitmap [r type bitmap:public:commands]
        assert_equal 2 [r bitcount bitmap:public:commands]
        assert_equal $::sparse_public_len [r bitop or bitmap:public:commands:copy bitmap:public:commands]
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

        # One native source makes the destination native, even overwriting
        # the previous string destination.
        r set bitop:dest:n1 [binary format H* f0]
        convert_string_bitmap_to_native r bitop:dest:n1
        assert_equal 1 [r bitop or bitop:dest:out bitop:dest:n1 bitop:dest:s2]
        assert_equal bitmap [r type bitop:dest:out]
        assert_equal [binary format H* ff] [r debug bitmap-raw bitop:dest:out]
    }

    test {BITOP destination is always native with bitmap-default-roaring yes} {
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

    test {BITOP NOT rejects oversized string sources when destination would be native} {
        set limit 1048576
        set oldval [config_get_set proto-max-bulk-len [expr {$limit + 1}]]
        r config set bitmap-default-roaring no
        r del bitop:not:mixed:big bitop:not:mixed:out
        r setbit bitop:not:mixed:big [expr {($limit + 1) * 8 - 1}] 1
        r config set proto-max-bulk-len $limit
        r config set bitmap-default-roaring yes

        assert_error {*string exceeds maximum allowed size (proto-max-bulk-len)*} {
            r bitop not bitop:not:mixed:out bitop:not:mixed:big
        }
        assert_equal 0 [r exists bitop:not:mixed:out]
        assert_equal string [r type bitop:not:mixed:big]

        r config set bitmap-default-roaring no
        r config set proto-max-bulk-len $oldval
        r del bitop:not:mixed:big
    }

    test {BITOP with sparse native sources computes in roaring space} {
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

    test {native bitmap helper exposes type encoding and exact raw bytes} {
        set raw [binary format H* 80400100080000]

        r set bitmap:raw $raw
        assert_equal [convert_string_bitmap_to_native r bitmap:raw] OK
        assert_equal [r type bitmap:raw] bitmap
        assert_equal [r object encoding bitmap:raw] bitmap-roaring
        assert_equal [r debug bitmap-raw bitmap:raw] $raw
        assert_error {WRONGTYPE*} {r get bitmap:raw}
    }

    test {native bitmap scan type and copy preserve bitmap objects} {
        set raw [binary format H* 010204000000]

        r set bitmap:copy-source $raw
        r set bitmap:string-peer value
        convert_string_bitmap_to_native r bitmap:copy-source
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

    test {native bitmap rejects generic string commands without materializing} {
        set raw [binary format H* 80400100080000]

        r set bitmap:string-boundary $raw
        convert_string_bitmap_to_native r bitmap:string-boundary
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

    test {plain SET overwrites a native bitmap key with a string} {
        set raw [binary format H* 80400100080000]

        r set bitmap:set-overwrite $raw
        convert_string_bitmap_to_native r bitmap:set-overwrite
        assert_equal bitmap [r type bitmap:set-overwrite]

        # Generic overwrite is the intended plain replacement path: SET
        # replaces a native bitmap like it replaces any other type, while
        # implicit string reads stay WRONGTYPE.
        r set bitmap:set-overwrite replacement
        assert_equal string [r type bitmap:set-overwrite]
        assert_equal replacement [r get bitmap:set-overwrite]
    }

    test {existence-conditional writes treat native bitmaps as existing keys} {
        set raw [binary format H* 80400100080000]

        r del bitmap:nx-boundary bitmap:nx-other
        r set bitmap:nx-boundary $raw
        convert_string_bitmap_to_native r bitmap:nx-boundary
        # NX-style writes check only existence, never type: a native bitmap
        # counts as existing and stays untouched.
        assert_equal 0 [r setnx bitmap:nx-boundary value]
        assert_equal 0 [r msetnx bitmap:nx-boundary value bitmap:nx-other other]
        assert_equal 0 [r exists bitmap:nx-other]
        assert_equal bitmap [r type bitmap:nx-boundary]
        assert_equal $raw [r debug bitmap-raw bitmap:nx-boundary]

        # SET ... XX overwrites a native bitmap like plain SET does.
        assert_equal OK [r set bitmap:nx-boundary replacement xx]
        assert_equal string [r type bitmap:nx-boundary]
        assert_equal replacement [r get bitmap:nx-boundary]
    }

    test {native bitmap stays opaque to additional string read surfaces} {
        set raw [binary format H* 80400100080000]

        r set bitmap:surface $raw
        r set bitmap:surface:string $raw
        convert_string_bitmap_to_native r bitmap:surface
        # MGET reports non-string keys as nil, native bitmaps included.
        assert_equal [list {} $raw] [r mget bitmap:surface bitmap:surface:string]
        # SUBSTR is the legacy alias of GETRANGE and stays WRONGTYPE.
        assert_error {WRONGTYPE*} {r substr bitmap:surface 0 -1}
        # LCS refuses non-string keys with its dedicated error.
        assert_error {*must contain string values*} {r lcs bitmap:surface bitmap:surface:string}

        assert_equal bitmap [r type bitmap:surface]
        assert_equal $raw [r debug bitmap-raw bitmap:surface]
    }

    test {SORT BY and GET patterns treat native bitmaps as missing values} {
        r del bitmap:sort:list
        r rpush bitmap:sort:list a b
        r set weight_a 2
        r set weight_b 1
        r set data_a string-a
        r set data_b string-b

        assert_equal {b a} [r sort bitmap:sort:list BY weight_* GET #]
        assert_equal {string-b string-a} [r sort bitmap:sort:list BY weight_* GET data_*]

        # lookupKeyByPattern() only dereferences OBJ_STRING values, so a
        # native bitmap weight or data target behaves exactly like a
        # missing key: no weight for BY (sorts as 0), nil for GET, and no
        # materialization back to a string.
        convert_string_bitmap_to_native r weight_a
        convert_string_bitmap_to_native r data_a
        assert_equal {a b} [r sort bitmap:sort:list BY weight_* GET #]
        assert_equal [list {} string-b] [r sort bitmap:sort:list BY weight_* GET data_*]
        assert_equal bitmap [r type weight_a]
        assert_equal bitmap [r type data_a]

        # The hash-field pattern branch ("BY pat->field") takes a separate
        # lookup path that requires OBJ_HASH; a native bitmap in pattern
        # position behaves like a missing key there too.
        r del wh_a wh_b
        r hset wh_b f 1
        r set wh_a placeholder
        convert_string_bitmap_to_native r wh_a
        assert_equal {a b} [r sort bitmap:sort:list BY wh_*->f GET #]
        assert_equal [list {} 1] [r sort bitmap:sort:list BY wh_*->f GET wh_*->f]
        assert_equal bitmap [r type wh_a]
    }

    test {Lua scripts observe native bitmaps through normal type checks} {
        set raw [binary format H* 80400100080000]

        r set bitmap:lua $raw
        convert_string_bitmap_to_native r bitmap:lua
        assert_equal 1 [r eval {return redis.call('getbit', KEYS[1], 0)} 1 bitmap:lua]
        assert_equal 4 [r eval {return redis.call('bitcount', KEYS[1])} 1 bitmap:lua]
        assert_error {*WRONGTYPE*} {r eval {return redis.call('get', KEYS[1])} 1 bitmap:lua}
        assert_equal bitmap [r type bitmap:lua]
    }

    test {native bitmap dump restore and debug reload preserve bitmap objects} {
        set raw [binary format H* f0000000000000010000]

        r set bitmap:persist $raw
        convert_string_bitmap_to_native r bitmap:persist
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

    test {RESTORE REPLACE preserves explicit string and native bitmap transitions} {
        set raw [binary format H* 80400100080000]

        r del bitmap:restore:source bitmap:restore:target
        r set bitmap:restore:source $raw
        set string_payload [r dump bitmap:restore:source]

        convert_string_bitmap_to_native r bitmap:restore:source
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

    test {native bitmap raw RDB restores run containers without capacity bloat} {
        set raw ""
        for {set i 0} {$i < 32} {incr i} {
            append raw [string repeat [binary format H* ff] 600]
            append raw [string repeat [binary format H* 00] 7592]
        }

        r del bitmap:rdb-run:a bitmap:rdb-run:b
        r set bitmap:rdb-run:a $raw
        convert_string_bitmap_to_native r bitmap:rdb-run:a
        set original_usage [r memory usage bitmap:rdb-run:a]

        r restore bitmap:rdb-run:b 0 [r dump bitmap:rdb-run:a]
        assert_equal bitmap [r type bitmap:rdb-run:b]
        assert_equal $raw [r debug bitmap-raw bitmap:rdb-run:b]

        set restored_usage [r memory usage bitmap:rdb-run:b]
        assert_lessthan_equal $restored_usage [expr {$original_usage + 8192}] \
            "restored_usage=$restored_usage original_usage=$original_usage"
        r del bitmap:rdb-run:a bitmap:rdb-run:b
    }

    test {native bitmap RDB uses compact payload for fragmented bitmaps} {
        set raw [string repeat [binary format H* 55] 8192]

        r del bitmap:rdb-frag:a bitmap:rdb-frag:b
        r set bitmap:rdb-frag:a $raw
        convert_string_bitmap_to_native r bitmap:rdb-frag:a
        set dump [r dump bitmap:rdb-frag:a]
        assert_lessthan [string length $dump] [expr {[string length $raw] + 128}] \
            "dump_len=[string length $dump] raw_len=[string length $raw]"

        r restore bitmap:rdb-frag:b 0 $dump
        assert_equal bitmap [r type bitmap:rdb-frag:b]
        assert_equal $raw [r debug bitmap-raw bitmap:rdb-frag:b]
        r del bitmap:rdb-frag:a bitmap:rdb-frag:b
    }

    test {native bitmap raw RDB payload keeps sparse bitmaps compact with compression} {
        set oldcomp [config_get_set rdbcompression yes]

        r del bitmap:rdb-sparse:string bitmap:rdb-sparse:native \
            bitmap:rdb-sparse:restored
        for {set i 0} {$i < 4096} {incr i} {
            r setbit bitmap:rdb-sparse:string [expr {$i * 4096}] 1
        }
        set raw [r get bitmap:rdb-sparse:string]
        r set bitmap:rdb-sparse:native $raw
        convert_string_bitmap_to_native r bitmap:rdb-sparse:native
        set native_dump [r dump bitmap:rdb-sparse:native]
        assert_lessthan [string length $native_dump] \
            [expr {[string length $raw] / 8}] \
            "native_dump_len=[string length $native_dump] raw_len=[string length $raw]"

        r restore bitmap:rdb-sparse:restored 0 $native_dump
        assert_equal bitmap [r type bitmap:rdb-sparse:restored]
        assert_equal bitmap-roaring [r object encoding bitmap:rdb-sparse:restored]
        assert_equal $raw [r debug bitmap-raw bitmap:rdb-sparse:restored]

        r del bitmap:rdb-sparse:string bitmap:rdb-sparse:native \
            bitmap:rdb-sparse:restored
        r config set rdbcompression $oldcomp
    }

    test {native bitmap raw RDB payload round-trips across internal shapes} {
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
            convert_string_bitmap_to_native r bitmap:endian:$name
            assert_equal [r debug bitmap-raw bitmap:endian:$name] $raw

            r restore bitmap:endian:restored:$name 0 [r dump bitmap:endian:$name]
            assert_equal bitmap [r type bitmap:endian:restored:$name]
            assert_equal [r debug bitmap-raw bitmap:endian:restored:$name] $raw
        }
    }

    test {native bitmap RDB save is not bounded by current proto-max-bulk-len} {
        set limit 1048576
        set byte_len [expr {$limit + 1}]
        set oldval [config_get_set proto-max-bulk-len [expr {$byte_len + 1024}]]
        set raw [string repeat [binary format H* 8000] [expr {$limit / 2}]]
        append raw [binary format H* 80]
        assert_equal $byte_len [string length $raw]

        r del bitmap:rdb-raw-bulk-limit
        r set bitmap:rdb-raw-bulk-limit $raw
        convert_string_bitmap_to_native r bitmap:rdb-raw-bulk-limit
        r config set proto-max-bulk-len $limit

        r debug reload

        r config set proto-max-bulk-len [expr {$byte_len + 1024}]
        assert_equal bitmap [r type bitmap:rdb-raw-bulk-limit]
        assert_equal bitmap-roaring [r object encoding bitmap:rdb-raw-bulk-limit]
        assert_equal $raw [r debug bitmap-raw bitmap:rdb-raw-bulk-limit]
        r del bitmap:rdb-raw-bulk-limit
        r config set proto-max-bulk-len $oldval
    }

    test {native bitmap RDB load is not bounded by current proto-max-bulk-len} {
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

    test {native bitmap unlink uses lazyfree for many roaring containers} {
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

    test {public-created native bitmaps survive debug reload} {
        r config set bitmap-default-roaring yes

        r setbit bitmap:public:reload:direct $::sparse_public_offset 1
        r set bitmap:public:reload:auto ""
        r setbit bitmap:public:reload:auto $::sparse_public_offset 1
        assert {[string length [r dump bitmap:public:reload:direct]] < 256}
        set digest_before [debug_digest]

        r debug reload

        assert_equal [debug_digest] $digest_before
        assert_equal bitmap [r type bitmap:public:reload:direct]
        assert_equal bitmap [r type bitmap:public:reload:auto]
        assert_equal 1 [r getbit bitmap:public:reload:direct $::sparse_public_offset]
        assert_equal 1 [r getbit bitmap:public:reload:auto $::sparse_public_offset]
        r config set bitmap-default-roaring no
    }
}

start_server {tags {"bitmap" "bitmap-native" "needs:debug" "modules" "external:skip" "cluster:skip"}} {
    r module load $testmodule

    test {module key API exposes bitmap without string access} {
        set raw [binary format H* 80400100080000]

        r set bitmap:module-boundary $raw
        r set bitmap:module-string $raw
        convert_string_bitmap_to_native r bitmap:module-boundary
        assert_equal {bitmap 7 0 0} [r test.key_string_api bitmap:module-boundary]
        assert_equal {string 7 1 1} [r test.key_string_api bitmap:module-string]
        assert_equal bitmap [r type bitmap:module-boundary]
        assert_equal $raw [r debug bitmap-raw bitmap:module-boundary]
        assert_equal string [r type bitmap:module-string]
        assert_equal $raw [r get bitmap:module-string]
    }

    test "Unload the module - misc" {
        assert_equal {OK} [r module unload misc]
    }
}

start_server {tags {"bitmap" "bitmap-native" "needs:debug" "external:skip" "cluster:skip" "logreqres:skip"} overrides {save {} aof-use-rdb-preamble no}} {
    test {native bitmap survives AOF rewrite as bitmap} {
        r config set appendonly yes
        r config set auto-aof-rewrite-percentage 0
        waitForBgrewriteaof r

        set raw [binary format H* 80000000000000000001]

        r set bitmap:aof $raw
        convert_string_bitmap_to_native r bitmap:aof
        set digest_before [debug_digest]

        r bgrewriteaof
        waitForBgrewriteaof r
        r debug loadaof

        assert_equal [debug_digest] $digest_before
        assert_equal [r type bitmap:aof] bitmap
        assert_equal [r object encoding bitmap:aof] bitmap-roaring
        assert_equal [r debug bitmap-raw bitmap:aof] $raw
    }

    test {public-created native bitmaps survive AOF rewrite as bitmap} {
        r flushall
        r config set appendonly yes
        waitForBgrewriteaof r
        r config set auto-aof-rewrite-percentage 0
        r config set bitmap-default-roaring yes

        r setbit bitmap:public:aof:direct $::sparse_public_offset 1
        r setbit bitmap:public:aof:zero 0 0
        r set bitmap:public:aof:auto ""
        r setbit bitmap:public:aof:auto $::sparse_public_offset 1
        set digest_before [debug_digest]

        r bgrewriteaof
        waitForBgrewriteaof r
        r debug loadaof

        assert_equal [debug_digest] $digest_before
        assert_equal bitmap [r type bitmap:public:aof:direct]
        assert_equal bitmap [r type bitmap:public:aof:zero]
        assert_equal bitmap [r type bitmap:public:aof:auto]
        assert_equal 1 [r getbit bitmap:public:aof:direct $::sparse_public_offset]
        assert_equal [binary format H* 00] [r debug bitmap-raw bitmap:public:aof:zero]
        assert_equal 1 [r getbit bitmap:public:aof:auto $::sparse_public_offset]
        r config set bitmap-default-roaring no
    }

    test {AOF rewrite preserves native and string bitmap objects} {
        r flushall
        r config set appendonly yes
        waitForBgrewriteaof r
        r config set auto-aof-rewrite-percentage 0

        set raw [binary format H* 80400100080000]
        r set bitmap:aof:transition:native $raw
        convert_string_bitmap_to_native r bitmap:aof:transition:native
        r set bitmap:aof:transition:string $raw

        assert_equal bitmap [r type bitmap:aof:transition:native]
        assert_equal string [r type bitmap:aof:transition:string]
        set digest_before [debug_digest]

        r bgrewriteaof
        waitForBgrewriteaof r
        r debug loadaof

        assert_equal [debug_digest] $digest_before
        assert_equal bitmap [r type bitmap:aof:transition:native]
        assert_equal bitmap-roaring [r object encoding bitmap:aof:transition:native]
        assert_equal $raw [r debug bitmap-raw bitmap:aof:transition:native]
        assert_equal string [r type bitmap:aof:transition:string]
        assert_equal $raw [r get bitmap:aof:transition:string]
    }
}

start_server {tags {"bitmap" "bitmap-native" "needs:debug" "external:skip" "cluster:skip" "logreqres:skip"} overrides {appendonly yes appendfsync always save {} aof-use-rdb-preamble no}} {
    test {native bitmap create and conversion are written to incremental AOF as RESTORE} {
        set aof [get_last_incr_aof_path r]
        set raw [binary format H* 80400100080000]

        r config set bitmap-default-roaring yes
        r setbit bitmap:aof-incr:create $::sparse_public_offset 1
        r config set bitmap-default-roaring no

        r set bitmap:aof-incr:convert $raw
        r pexpire bitmap:aof-incr:convert 600000
        convert_string_bitmap_to_native r bitmap:aof-incr:convert

        set fp [open $aof r]
        fconfigure $fp -translation binary
        fconfigure $fp -blocking 1

        set commands {}
        while {1} {
            set cmd [read_from_aof $fp]
            if {$cmd eq ""} break
            lappend commands $cmd
        }
        close $fp

        set forbidden {}
        set create_restore 0
        set convert_restore 0
        foreach cmd $commands {
            set name [lindex $cmd 0]
            if {$name eq "setbit" || $name eq "bitmap"} {
                lappend forbidden $cmd
            }
            if {$name eq "restore"} {
                assert_equal REPLACE [lindex $cmd 4]
                set key [lindex $cmd 1]
                if {$key eq "bitmap:aof-incr:create"} {
                    assert_equal 5 [llength $cmd]
                    assert_equal 0 [lindex $cmd 2]
                    incr create_restore
                } elseif {$key eq "bitmap:aof-incr:convert"} {
                    assert_equal 6 [llength $cmd]
                    assert_equal ABSTTL [lindex $cmd 5]
                    incr convert_restore
                }
            }
        }

        assert_equal {} $forbidden
        assert_equal 1 $create_restore
        assert_equal 1 $convert_restore

        set digest_before [debug_digest]
        r debug loadaof
        assert_equal $digest_before [debug_digest]
        assert_equal bitmap [r type bitmap:aof-incr:create]
        assert_equal bitmap [r type bitmap:aof-incr:convert]
        assert_equal $raw [r debug bitmap-raw bitmap:aof-incr:convert]
    }
}

start_server {tags {"bitmap" "bitmap-native" "repl" "external:skip" "cluster:skip"}} {
    start_server {} {
        set master [srv -1 client]
        set master_host [srv -1 host]
        set master_port [srv -1 port]
        set replica [srv 0 client]

        $replica replicaof $master_host $master_port
        wait_for_sync $replica
        wait_for_ofs_sync $master $replica

        test {native bitmap public creation replicates deterministic type transitions} {
            # The replica stays in bitmap-default-roaring no: type decisions must arrive
            # from the master as explicit RESTOREs, never be re-derived from
            # replica-local configuration.
            $master config set bitmap-default-roaring yes
            $replica config set bitmap-default-roaring no

            $master setbit bitmap:public:repl:direct $::sparse_public_offset 1
            $master setbit bitmap:public:repl:zero 0 0
            $master set bitmap:public:repl:auto ""
            $master setbit bitmap:public:repl:auto $::sparse_public_offset 1
            wait_for_ofs_sync $master $replica

            assert_equal bitmap [$replica type bitmap:public:repl:direct]
            assert_equal bitmap [$replica type bitmap:public:repl:zero]
            assert_equal bitmap [$replica type bitmap:public:repl:auto]
            assert_equal 1 [$replica getbit bitmap:public:repl:direct $::sparse_public_offset]
            assert_equal [binary format H* 00] [$replica debug bitmap-raw bitmap:public:repl:zero]
            assert_equal 1 [$replica getbit bitmap:public:repl:auto $::sparse_public_offset]
            assert_error {WRONGTYPE*} {$replica get bitmap:public:repl:direct}
            assert_error {WRONGTYPE*} {$replica get bitmap:public:repl:auto}
            assert_equal [$master debug digest] [$replica debug digest]
        }

        test {plain SETBIT on an existing native bitmap replicates as a command} {
            # After the explicit RESTORE transition, later writes replicate
            # as plain SETBITs against the same type on both sides.
            $master setbit bitmap:public:repl:direct 12345 1
            wait_for_ofs_sync $master $replica

            assert_equal bitmap [$replica type bitmap:public:repl:direct]
            assert_equal 1 [$replica getbit bitmap:public:repl:direct 12345]
        }

        test {BITOP destinations replicate deterministically across modes} {
            # String-only sources with a bitmap-default-roaring yes master: the native
            # destination decision is master-local, so the result arrives as
            # a RESTORE and the replica converges although it would have
            # produced a string itself.
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

        test {native bitmaps survive a full resync as bitmaps} {
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
            assert_equal 1 [$replica getbit bitmap:public:repl:direct $::sparse_public_offset]
            assert_equal 1 [$replica getbit bitmap:public:repl:direct 12345]
            assert_equal [binary format H* 00] [$replica debug bitmap-raw bitmap:public:repl:zero]
            assert_equal 1 [$replica getbit bitmap:public:repl:auto $::sparse_public_offset]
            assert_equal [$master debug digest] [$replica debug digest]
        }

        test {bitmap-default-roaring conversion replicates as RESTORE} {
            set raw [binary format H* 80400100080000]

            $master config set bitmap-default-roaring no
            $master set bitmap:public:repl:conv $raw
            wait_for_ofs_sync $master $replica
            assert_equal string [$replica type bitmap:public:repl:conv]

            # The conversion must arrive as the RESTORE effect, never as a
            # replayed SETBIT: whether the replica would choose native depends
            # on its own configuration.
            convert_string_bitmap_to_native $master bitmap:public:repl:conv
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
            convert_string_bitmap_to_native $master bitmap:public:repl:restore:source
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
    set left "bitmap:native:translated:jaccard:$name:left"
    set right "bitmap:native:translated:jaccard:$name:right"
    set intersection "bitmap:native:translated:jaccard:$name:intersection"
    set union "bitmap:native:translated:jaccard:$name:union"

    seed_native_bitmap $left $left_bits
    seed_native_bitmap $right $right_bits

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

proc assert_native_bitop_matches_string {name op source_bitsets} {
    set string_dest "bitmap:native:bitop:$name:string:dest"
    set native_dest "bitmap:native:bitop:$name:native:dest"
    set string_sources {}
    set native_sources {}

    for {set i 0} {$i < [llength $source_bitsets]} {incr i} {
        set string_key "bitmap:native:bitop:$name:string:src:$i"
        set native_key "bitmap:native:bitop:$name:native:src:$i"
        seed_string_bitmap $string_key [lindex $source_bitsets $i]
        seed_native_bitmap $native_key [lindex $source_bitsets $i]
        lappend string_sources $string_key
        lappend native_sources $native_key
    }

    set string_reply [r bitop $op $string_dest {*}$string_sources]
    set native_reply [r bitop $op $native_dest {*}$native_sources]
    assert_equal $string_reply $native_reply
    assert_equal [bitmap_logical_raw $string_dest] [bitmap_logical_raw $native_dest]
    assert_equal $string_reply [string length [bitmap_logical_raw $string_dest]]
    assert_equal $native_reply [string length [bitmap_logical_raw $native_dest]]
    if {[r exists $native_dest]} {
        # At least one native source makes the destination native.
        assert_equal bitmap [r type $native_dest]
        assert_equal string [r type $string_dest]
    }
}

proc assert_native_bitop_bitset_case {name op source_bitsets expected_bits {missing_indexes {}} {alias_index -1} {dest_seed __none__}} {
    set string_dest "bitmap:native:bitop:case:$name:string:dest"
    set native_dest "bitmap:native:bitop:case:$name:native:dest"
    set string_sources {}
    set native_sources {}
    set string_source_raws {}
    set native_source_raws {}

    r config set bitmap-default-roaring no

    if {$dest_seed eq "__none__"} {
        r del $string_dest $native_dest
    } else {
        seed_string_bitmap $string_dest $dest_seed
        seed_native_bitmap $native_dest $dest_seed
    }

    for {set i 0} {$i < [llength $source_bitsets]} {incr i} {
        set string_key "bitmap:native:bitop:case:$name:string:src:$i"
        set native_key "bitmap:native:bitop:case:$name:native:src:$i"
        if {[lsearch -exact $missing_indexes $i] >= 0} {
            r del $string_key $native_key
        } else {
            seed_string_bitmap $string_key [lindex $source_bitsets $i]
            seed_native_bitmap $native_key [lindex $source_bitsets $i]
        }
        lappend string_sources $string_key
        lappend native_sources $native_key
        lappend string_source_raws [bitmap_logical_raw $string_key]
        lappend native_source_raws [bitmap_logical_raw $native_key]
    }

    if {$alias_index >= 0} {
        set string_dest [lindex $string_sources $alias_index]
        set native_dest [lindex $native_sources $alias_index]
    }

    set string_reply [r bitop $op $string_dest {*}$string_sources]
    set native_reply [r bitop $op $native_dest {*}$native_sources]
    assert_equal $string_reply $native_reply
    assert_equal [bitmap_logical_raw $string_dest] [bitmap_logical_raw $native_dest]
    assert_equal $string_reply [string length [bitmap_logical_raw $string_dest]]
    assert_equal $native_reply [string length [bitmap_logical_raw $native_dest]]
    assert_bitmap_has_exact_bits $string_dest $expected_bits
    assert_bitmap_has_exact_bits $native_dest $expected_bits
    if {[r exists $native_dest]} {
        assert_equal bitmap [r type $native_dest]
        assert_equal bitmap-roaring [r object encoding $native_dest]
    }

    for {set i 0} {$i < [llength $source_bitsets]} {incr i} {
        if {$i == $alias_index} continue
        assert_equal [lindex $string_source_raws $i] [bitmap_logical_raw [lindex $string_sources $i]]
        assert_equal [lindex $native_source_raws $i] [bitmap_logical_raw [lindex $native_sources $i]]
    }
}

proc assert_native_bitop_raws_match_string {name op source_raws native_indexes {alias_index -1}} {
    set string_dest "bitmap:native:bitop:$name:string:dest"
    set native_dest "bitmap:native:bitop:$name:native:dest"
    set string_sources {}
    set native_sources {}
    set string_source_raws {}
    set native_source_raws {}

    r config set bitmap-default-roaring no

    for {set i 0} {$i < [llength $source_raws]} {incr i} {
        set string_key "bitmap:native:bitop:$name:string:src:$i"
        set native_key "bitmap:native:bitop:$name:native:src:$i"
        r set $string_key [lindex $source_raws $i]
        r set $native_key [lindex $source_raws $i]
        if {[lsearch -exact $native_indexes $i] >= 0} {
            convert_string_bitmap_to_native r $native_key
        }
        lappend string_sources $string_key
        lappend native_sources $native_key
        lappend string_source_raws [bitmap_logical_raw $string_key]
        lappend native_source_raws [bitmap_logical_raw $native_key]
    }

    if {$alias_index >= 0} {
        set string_dest [lindex $string_sources $alias_index]
        set native_dest [lindex $native_sources $alias_index]
    }

    set string_reply [r bitop $op $string_dest {*}$string_sources]
    set native_reply [r bitop $op $native_dest {*}$native_sources]
    assert_equal $string_reply $native_reply
    assert_equal [bitmap_logical_raw $string_dest] [bitmap_logical_raw $native_dest]
    assert_equal $string_reply [string length [bitmap_logical_raw $string_dest]]
    assert_equal $native_reply [string length [bitmap_logical_raw $native_dest]]
    if {[r exists $native_dest] && [llength $native_indexes] > 0} {
        assert_equal bitmap [r type $native_dest]
        assert_equal string [r type $string_dest]
    }

    for {set i 0} {$i < [llength $source_raws]} {incr i} {
        if {$i == $alias_index} continue
        assert_equal [lindex $string_source_raws $i] [bitmap_logical_raw [lindex $string_sources $i]]
        assert_equal [lindex $native_source_raws $i] [bitmap_logical_raw [lindex $native_sources $i]]
    }
}

proc assert_native_bitmap_command_matches_string {name raw command} {
    set string_key "bitmap:native:read-edge:$name:string"
    set native_key "bitmap:native:read-edge:$name:native"
    r set $string_key $raw
    r set $native_key $raw
    convert_string_bitmap_to_native r $native_key
    set string_cmd [lreplace $command 1 1 $string_key]
    set native_cmd [lreplace $command 1 1 $native_key]
    assert_equal [r {*}$string_cmd] [r {*}$native_cmd]
    assert_equal bitmap [r type $native_key]
    assert_equal bitmap-roaring [r object encoding $native_key]
}

proc assert_native_bitmap_write_matches_string {name raw command} {
    set string_key "bitmap:native:write-edge:$name:string"
    set native_key "bitmap:native:write-edge:$name:native"
    r set $string_key $raw
    r set $native_key $raw
    convert_string_bitmap_to_native r $native_key
    set string_cmd [lreplace $command 1 1 $string_key]
    set native_cmd [lreplace $command 1 1 $native_key]
    assert_equal [r {*}$string_cmd] [r {*}$native_cmd]
    assert_equal [bitmap_logical_raw $string_key] [r debug bitmap-raw $native_key]
    assert_equal bitmap [r type $native_key]
    assert_equal bitmap-roaring [r object encoding $native_key]
}

start_server {tags {"bitmap" "bitmap-native" "needs:debug" "cluster:skip"}} {
    test {native bitmap read commands preserve type encoding and bytes} {
        set raw [binary format H* 80400100080000]

        r set bitmap:native:read $raw
        convert_string_bitmap_to_native r bitmap:native:read
        assert_equal 1 [r getbit bitmap:native:read 0]
        assert_equal 1 [r getbit bitmap:native:read 9]
        assert_equal 0 [r getbit bitmap:native:read 10]
        assert_equal 4 [r bitcount bitmap:native:read]
        assert_equal 2 [r bitcount bitmap:native:read 8 23 bit]
        assert_equal 0 [r bitpos bitmap:native:read 1]
        assert_equal 1 [r bitpos bitmap:native:read 0]
        assert_equal 9 [r bitpos bitmap:native:read 1 8 -1 bit]
        assert_equal {1 1 1} [r bitfield_ro bitmap:native:read GET u1 0 GET u1 9 GET u1 36]
        assert_error {ERR BITFIELD_RO only supports the GET subcommand} {
            r bitfield_ro bitmap:native:read SET u8 0 255
        }

        assert_equal bitmap [r type bitmap:native:read]
        assert_equal bitmap-roaring [r object encoding bitmap:native:read]
        assert_equal $raw [r debug bitmap-raw bitmap:native:read]
    }

    test {bitmap-default-roaring conversion preserves dense raw chunks and boundary bits} {
        set raw [binary format H* "[string repeat ff 8192]8001"]

        r set bitmap:native:convert:dense $raw
        convert_string_bitmap_to_native r bitmap:native:convert:dense
        assert_equal bitmap [r type bitmap:native:convert:dense]
        assert_equal bitmap-roaring [r object encoding bitmap:native:convert:dense]
        assert_equal $raw [r debug bitmap-raw bitmap:native:convert:dense]
        assert_equal 65538 [r bitcount bitmap:native:convert:dense]
        assert_equal 1 [r getbit bitmap:native:convert:dense 0]
        assert_equal 1 [r getbit bitmap:native:convert:dense 65535]
        assert_equal 1 [r getbit bitmap:native:convert:dense 65536]
        assert_equal 1 [r getbit bitmap:native:convert:dense 65551]
    }

    test {SETBIT and GETBIT round trip native bitmap offsets} {
        seed_native_bitmap bitmap:native:setbit:loop {}

        for {set offset 0} {$offset < 100} {incr offset} {
            assert_equal 0 [r setbit bitmap:native:setbit:loop $offset 1]
            assert_equal 1 [r getbit bitmap:native:setbit:loop $offset]
            assert_equal 1 [r setbit bitmap:native:setbit:loop $offset 0]
            assert_equal 0 [r getbit bitmap:native:setbit:loop $offset]
        }

        assert_equal bitmap [r type bitmap:native:setbit:loop]
        assert_equal bitmap-roaring [r object encoding bitmap:native:setbit:loop]
        assert_equal 0 [r bitcount bitmap:native:setbit:loop]
    }

    test {SETBIT updates existing native bitmap keys through direct native path} {
        r config set bitmap-default-roaring yes
        r del bitmap:native:setbit:existing

        assert_equal 0 [r setbit bitmap:native:setbit:existing 5 1]
        assert_equal bitmap [r type bitmap:native:setbit:existing]

        r config set bitmap-default-roaring no
        assert_equal 0 [r setbit bitmap:native:setbit:existing 6 1]
        assert_equal bitmap [r type bitmap:native:setbit:existing]
        assert_equal bitmap-roaring [r object encoding bitmap:native:setbit:existing]
        assert_equal 1 [r getbit bitmap:native:setbit:existing 6]
        assert_equal 2 [r bitcount bitmap:native:setbit:existing]
    }

    test {SETBIT updates native bitmap values and preserves trailing zero length} {
        r set bitmap:native:setbit [binary format H* 8000]
        convert_string_bitmap_to_native r bitmap:native:setbit
        assert_equal 0 [r setbit bitmap:native:setbit 9 1]
        assert_equal bitmap [r type bitmap:native:setbit]
        assert_equal bitmap-roaring [r object encoding bitmap:native:setbit]
        assert_equal [binary format H* 8040] [r debug bitmap-raw bitmap:native:setbit]

        assert_equal 0 [r setbit bitmap:native:setbit 23 0]
        assert_equal bitmap [r type bitmap:native:setbit]
        assert_equal [binary format H* 804000] [r debug bitmap-raw bitmap:native:setbit]

        assert_equal 1 [r setbit bitmap:native:setbit 0 0]
        assert_equal bitmap [r type bitmap:native:setbit]
        assert_equal [binary format H* 004000] [r debug bitmap-raw bitmap:native:setbit]
    }

    test {native bitmap MEMORY USAGE tracks roaring container allocation updates} {
        r config set bitmap-default-roaring yes
        r del bitmap:native:memory

        assert_equal 0 [r setbit bitmap:native:memory 0 1]
        set one_container [r memory usage bitmap:native:memory]
        assert_morethan $one_container 0

        assert_equal 0 [r setbit bitmap:native:memory 65536 1]
        set two_containers [r memory usage bitmap:native:memory]
        assert_morethan $two_containers $one_container

        assert_equal 1 [r setbit bitmap:native:memory 65536 0]
        set back_to_one [r memory usage bitmap:native:memory]
        assert_lessthan $back_to_one $two_containers
        assert_equal 1 [r bitcount bitmap:native:memory]

        assert_equal 1 [r setbit bitmap:native:memory 0 0]
        set empty [r memory usage bitmap:native:memory]
        assert_lessthan $empty $back_to_one
        assert_equal 0 [r bitcount bitmap:native:memory]

        r del bitmap:native:memory:same-container
        assert_equal 0 [r setbit bitmap:native:memory:same-container 0 1]
        set sparse_container [r memory usage bitmap:native:memory:same-container]
        for {set bit 1} {$bit <= 4096} {incr bit} {
            assert_equal 0 [r setbit bitmap:native:memory:same-container $bit 1]
        }
        set dense_container [r memory usage bitmap:native:memory:same-container]
        assert_morethan $dense_container $sparse_container
        assert_equal 4097 [r bitcount bitmap:native:memory:same-container]

        r config set bitmap-default-roaring no
        r del bitmap:native:memory bitmap:native:memory:same-container
    }

    test {bitmap commands operate on legacy and native representations with default native creation disabled} {
        r config set bitmap-default-roaring no
        set raw [binary format H* 804001]
        set string_key bitmap:native:mixed-surface:string
        set native_key bitmap:native:mixed-surface:native

        r set $string_key $raw
        r set $native_key $raw
        convert_string_bitmap_to_native r $native_key
        assert_equal string [r type $string_key]
        assert_equal bitmap [r type $native_key]
        assert_equal bitmap-roaring [r object encoding $native_key]

        assert_equal [r setbit $string_key 23 1] [r setbit $native_key 23 1]
        assert_equal [r getbit $string_key 23] [r getbit $native_key 23]
        assert_equal [r bitcount $string_key] [r bitcount $native_key]
        assert_equal [r bitcount $string_key 3 20 bit] [r bitcount $native_key 3 20 bit]
        assert_equal [r bitpos $string_key 1] [r bitpos $native_key 1]
        assert_equal [r bitpos $string_key 0 4 -1 bit] [r bitpos $native_key 0 4 -1 bit]

        set bitfield_cmd {GET u8 0 SET u5 9 17 INCRBY i6 16 -3 GET i6 16}
        assert_equal [r bitfield $string_key {*}$bitfield_cmd] [r bitfield $native_key {*}$bitfield_cmd]
        assert_equal [r bitfield_ro $string_key GET u8 0 GET u8 16] [r bitfield_ro $native_key GET u8 0 GET u8 16]
        assert_equal [r get $string_key] [r debug bitmap-raw $native_key]

        assert_native_bitop_raws_match_string mixed-surface:bitop or \
            [list [r get $string_key] [binary format H* 0f00ff]] {0}
    }

    test {GETBIT past the native bitmap logical length returns 0} {
        seed_native_bitmap bitmap:native:getbit:past {3}

        assert_equal 1 [r getbit bitmap:native:getbit:past 3]
        assert_equal 0 [r getbit bitmap:native:getbit:past 7]
        assert_equal 0 [r getbit bitmap:native:getbit:past 100]
        assert_equal 0 [r getbit bitmap:native:getbit:past 4294967295]
        assert_error {*bit offset is*out of range*} {
            r getbit bitmap:native:getbit:past 4294967296
        }
        assert_error {*bit offset is*out of range*} {
            r getbit bitmap:native:getbit:past 9223372036854775808
        }
        assert_equal [binary format H* 10] [r debug bitmap-raw bitmap:native:getbit:past]
    }

    test {native bitmap v1 cap applies when proto-max-bulk-len permits wider strings} {
        set max_native_bit 4294967295
        set first_rejected [expr {$max_native_bit + 1}]
        set limit [expr {($first_rejected / 8) + 1}]
        set oldval [config_get_set proto-max-bulk-len $limit]
        r config set bitmap-default-roaring yes
        r del bitmap:native:wide-offset-cap

        assert_equal 0 [r setbit bitmap:native:wide-offset-cap $max_native_bit 1]
        assert_equal bitmap [r type bitmap:native:wide-offset-cap]
        assert_equal bitmap-roaring [r object encoding bitmap:native:wide-offset-cap]
        assert_equal 1 [r getbit bitmap:native:wide-offset-cap $max_native_bit]
        assert_equal {1} [r bitfield_ro bitmap:native:wide-offset-cap GET u1 $max_native_bit]
        assert_equal 1 [r bitcount bitmap:native:wide-offset-cap]

        # Offsets follow the current proto-max-bulk-len exactly like string
        # bitmaps: lowering it below existing data bounds later accesses too.
        r config set proto-max-bulk-len 1048576
        foreach cmd [list \
            [list getbit bitmap:native:wide-offset-cap $max_native_bit] \
            [list getbit bitmap:native:wide-offset-cap $first_rejected] \
            [list bitfield_ro bitmap:native:wide-offset-cap GET u1 $first_rejected] \
            [list setbit bitmap:native:wide-offset-cap $first_rejected 1] \
            [list bitfield bitmap:native:wide-offset-cap SET u1 $first_rejected 1] \
        ] {
            assert_error {*bit offset*out of range*} {r {*}$cmd}
        }
        assert_equal 1 [r bitcount bitmap:native:wide-offset-cap]

        r config set bitmap-default-roaring no
        r config set proto-max-bulk-len $oldval
        r del bitmap:native:wide-offset-cap
    }

    test {SETBIT keeps the proto-max-bulk-len offset limit on native bitmaps} {
        seed_native_bitmap bitmap:native:setbit:cap {0}

        assert_error {*bit offset is*out of range*} {
            r setbit bitmap:native:setbit:cap 4294967296 1
        }
        assert_equal bitmap [r type bitmap:native:setbit:cap]
        assert_equal 1 [r bitcount bitmap:native:setbit:cap]
        r del bitmap:native:setbit:cap
    }

    test {native bitmap BITCOUNT and BITPOS cover redis-roaring integration cases} {
        seed_native_bitmap bitmap:native:countpos:fib {1 2 3 5 8 13}
        assert_equal 6 [r bitcount bitmap:native:countpos:fib]
        assert_equal 1 [r bitpos bitmap:native:countpos:fib 1]
        assert_equal 0 [r bitpos bitmap:native:countpos:fib 0]

        seed_native_bitmap bitmap:native:countpos:first-one {3 4 6 10 12}
        assert_equal 3 [r bitpos bitmap:native:countpos:first-one 1]

        seed_native_bitmap bitmap:native:countpos:first-zero {0 1 2 3 4 6}
        assert_equal 5 [r bitpos bitmap:native:countpos:first-zero 0]

        seed_native_bitmap bitmap:native:countpos:empty {}
        assert_equal -1 [r bitpos bitmap:native:countpos:empty 0]
        assert_equal -1 [r bitpos bitmap:native:countpos:empty 1]

        seed_native_bitmap bitmap:native:countpos:single-zero {0}
        assert_equal 1 [r bitpos bitmap:native:countpos:single-zero 0]
    }

    test {native bitmap BITCOUNT and BITPOS match string edge ranges} {
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
            assert_native_bitmap_command_matches_string "mixed:$idx" $raw $command
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
            assert_native_bitmap_command_matches_string "ones:$idx" $all_ones $command
            incr idx
        }
    }

    test {native bitmap BITCOUNT and BITPOS handle container edges} {
        # Bits in distinct 2^16 containers, plus dense runs, exercise the
        # container-walking BITPOS code where uint32 and uint64 arithmetic mix.
        seed_native_bitmap bitmap:native:cap-edge {0}
        r setbit bitmap:native:cap-edge 65535 1
        r setbit bitmap:native:cap-edge 65536 1
        r setbit bitmap:native:cap-edge 131071 1

        assert_equal 4 [r bitcount bitmap:native:cap-edge]
        assert_equal 65535 [r bitpos bitmap:native:cap-edge 1 1 -1 bit]
        assert_equal 65535 [r bitpos bitmap:native:cap-edge 1 8191]
        assert_equal 131071 [r bitpos bitmap:native:cap-edge 1 65537 -1 bit]
        assert_equal 1 [r bitpos bitmap:native:cap-edge 0]
        assert_equal -1 [r bitpos bitmap:native:cap-edge 0 65535 65535 bit]
        assert_equal 2 [r bitcount bitmap:native:cap-edge 65535 65536 bit]
        r del bitmap:native:cap-edge

        # A dense run crossing a container boundary: the first clear bit
        # after the run must come from the container-level scan. With an
        # explicit BIT range every bit is set, so the reply is -1; without an
        # explicit end the logical length supplies the imaginary trailing
        # zero at bit 65568.
        seed_native_bitmap bitmap:native:run-edge {}
        r bitfield bitmap:native:run-edge SET i64 65504 -1
        assert_equal {-1} [r bitfield_ro bitmap:native:run-edge GET i64 65504]
        assert_equal 65504 [r bitpos bitmap:native:run-edge 1]
        assert_equal -1 [r bitpos bitmap:native:run-edge 0 65504 -1 bit]
        assert_equal 65568 [r bitpos bitmap:native:run-edge 0 8188]
        assert_equal 64 [r bitcount bitmap:native:run-edge]
        r del bitmap:native:run-edge
    }

    test {BITFIELD writes native bitmap values through the direct write path} {
        r set bitmap:native:bitfield [binary format H* 00]
        convert_string_bitmap_to_native r bitmap:native:bitfield
        assert_equal {0 15} [r bitfield bitmap:native:bitfield SET u4 4 15 GET u8 0]
        assert_equal bitmap [r type bitmap:native:bitfield]
        assert_equal bitmap-roaring [r object encoding bitmap:native:bitfield]
        assert_equal [binary format H* 0f] [r debug bitmap-raw bitmap:native:bitfield]
        assert_equal {15} [r bitfield_ro bitmap:native:bitfield GET u4 4]

        assert_equal {0} [r bitfield bitmap:native:bitfield SET u1 23 0]
        assert_equal bitmap [r type bitmap:native:bitfield]
        assert_equal [binary format H* 0f0000] [r debug bitmap-raw bitmap:native:bitfield]

        seed_native_bitmap bitmap:native:bitfield:clear {0}
        assert_equal {2} [r bitfield bitmap:native:bitfield:clear SET u2 0 0]
        assert_equal 0 [r bitcount bitmap:native:bitfield:clear]
        assert_equal [binary format H* 00] [r debug bitmap-raw bitmap:native:bitfield:clear]
    }

    test {BITFIELD signed INCRBY preserves native bitmap values} {
        seed_native_bitmap bitmap:native:bitfield:signed {}

        assert_equal {0 1 1} [r bitfield bitmap:native:bitfield:signed SET i5 0 -1 INCRBY i5 0 2 GET i5 0]
        assert_equal bitmap [r type bitmap:native:bitfield:signed]
        assert_equal bitmap-roaring [r object encoding bitmap:native:bitfield:signed]
        assert_equal {1} [r bitfield_ro bitmap:native:bitfield:signed GET i5 0]
    }

    test {native bitmap BITFIELD direct paths match string edge cases} {
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
            assert_native_bitmap_write_matches_string $idx $raw $command
            incr idx
        }

        assert_native_bitmap_write_matches_string grow-after-failed-high-write \
            [binary format H* 00] \
            {bitfield key SET u1 0 1 OVERFLOW FAIL SET u2 47 5}

        assert_native_bitmap_write_matches_string grow-after-fail-only-high-write \
            [binary format H* 00] \
            {bitfield key OVERFLOW FAIL SET u2 47 5}

        set fail_key bitmap:native:bitfield:overflow-fail-string-growth
        r config set bitmap-default-roaring no
        r set $fail_key [binary format H* 00]
        assert_equal string [r type $fail_key]
        assert_equal {{}} [r bitfield $fail_key OVERFLOW FAIL SET u2 47 5]
        assert_equal string [r type $fail_key]
        assert_equal 7 [r strlen $fail_key]
        assert_equal [binary format H* 00000000000000] [r get $fail_key]

        set watched_fail_key bitmap:native:bitfield:overflow-fail-string-growth-watch
        r set $watched_fail_key [binary format H* 00]
        r watch $watched_fail_key
        assert_equal {{}} [r bitfield $watched_fail_key OVERFLOW FAIL SET u2 47 5]
        r multi
        r ping
        assert_equal {} [r exec]
        assert_equal 7 [r strlen $watched_fail_key]
    }

    test {BITFIELD uses the same offset limit for string and native bitmaps} {
        set limit 1048576
        set oldval [config_get_set proto-max-bulk-len $limit]
        set limit_bits [expr {$limit * 8}]
        set last_allowed [expr {$limit_bits - 1}]

        seed_string_bitmap bitmap:string:bitfield:limit {}
        seed_native_bitmap bitmap:native:bitfield:limit {}

        foreach key {bitmap:string:bitfield:limit bitmap:native:bitfield:limit} {
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
        assert_equal bitmap [r type bitmap:native:bitfield:limit]
        assert_equal bitmap-roaring [r object encoding bitmap:native:bitfield:limit]

        r config set proto-max-bulk-len $oldval
        r del bitmap:string:bitfield:limit bitmap:native:bitfield:limit
    }

    test {translated redis-roaring int-array bit-array and clear scenarios use core bitmap commands} {
        r config set bitmap-default-roaring yes

        set int_key bitmap:native:translated:int-array
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

        set range_key bitmap:native:translated:range-array
        r del $range_key
        foreach bit {0 8 16} {
            assert_equal 0 [r setbit $range_key $bit 1]
        }
        assert_bitmap_has_exact_bits $range_key {0 8 16}
        assert_equal {1 1 1} [r bitfield_ro $range_key GET u1 0 GET u1 8 GET u1 16]
        assert_equal 3 [r bitcount $range_key 0 16 bit]

        set bitarray_key bitmap:native:translated:bit-array
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

        set range_key bitmap:native:translated:setrange
        r del $range_key
        for {set bit 0} {$bit < 5} {incr bit} {
            assert_equal 0 [r setbit $range_key $bit 1]
        }
        assert_bitmap_has_exact_bits $range_key {0 1 2 3 4}
        assert_equal 0 [r bitpos $range_key 1]
        assert_equal 5 [r bitpos $range_key 0]

        set full_key bitmap:native:translated:setfull
        r set $full_key [binary format H* ff]
        convert_string_bitmap_to_native r $full_key
        assert_equal bitmap [r type $full_key]
        assert_equal bitmap-roaring [r object encoding $full_key]
        assert_bitmap_has_exact_bits $full_key {0 1 2 3 4 5 6 7}
        assert_equal 8 [r bitpos $full_key 0]

        set minmax_key bitmap:native:translated:minmax
        seed_native_bitmap $minmax_key {}
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
        set a bitmap:native:translated:contains:a
        set b bitmap:native:translated:contains:b
        set c bitmap:native:translated:contains:c
        set e bitmap:native:translated:contains:empty

        seed_native_bitmap $a {1 2 3 4 5}
        seed_native_bitmap $b {2 3}
        seed_native_bitmap $c {3 4 6}
        seed_native_bitmap $e {}

        r bitop and bitmap:native:translated:contains:some $a $b
        assert_equal 2 [r bitcount bitmap:native:translated:contains:some]

        r bitop and bitmap:native:translated:contains:none $a bitmap:native:translated:contains:missing
        assert_equal 0 [r bitcount bitmap:native:translated:contains:none]

        r bitop diff bitmap:native:translated:contains:subset-miss $b $a
        assert_equal 0 [r bitcount bitmap:native:translated:contains:subset-miss]
        assert {[r bitcount $b] < [r bitcount $a]}

        r bitop diff bitmap:native:translated:contains:not-subset $c $a
        assert_bitmap_has_exact_bits bitmap:native:translated:contains:not-subset {6}

        seed_native_bitmap bitmap:native:translated:contains:eq1 {1 2 3 4 5}
        seed_native_bitmap bitmap:native:translated:contains:eq2 {1 2 3 4 5}
        r bitop xor bitmap:native:translated:contains:eq-diff \
            bitmap:native:translated:contains:eq1 bitmap:native:translated:contains:eq2
        assert_equal 0 [r bitcount bitmap:native:translated:contains:eq-diff]

        r bitop diff bitmap:native:translated:contains:empty-subset $e $a
        assert_equal 0 [r bitcount bitmap:native:translated:contains:empty-subset]

        assert_bitmap_translated_jaccard overlap {1 2 3 4 5} {3 4 5 6 7} 3 7 0.428571
        assert_bitmap_translated_jaccard subset {1 2 3} {1 2 3 4 5} 3 5 0.600000
        assert_bitmap_translated_jaccard identical {8 13 21} {8 13 21} 3 3 1.000000
        assert_bitmap_translated_jaccard one-empty {1 2 3} {} 0 3 0.000000
        assert_bitmap_translated_jaccard disjoint {1 2} {3 4} 0 4 0.000000
        assert_bitmap_translated_jaccard empty {} {} 0 0 -1
    }

    test {BITOP stores native destinations when sources include native bitmaps} {
        r set bitmap:native:bitop:a [binary format H* f000]
        convert_string_bitmap_to_native r bitmap:native:bitop:a
        r set bitmap:native:bitop:b [binary format H* 0fff]
        r set bitmap:native:bitop:dest [binary format H* aa]
        convert_string_bitmap_to_native r bitmap:native:bitop:dest
        assert_equal 2 [r bitop or bitmap:native:bitop:dest bitmap:native:bitop:a bitmap:native:bitop:b]
        assert_equal bitmap [r type bitmap:native:bitop:dest]
        assert_equal [binary format H* ffff] [r debug bitmap-raw bitmap:native:bitop:dest]

        assert_equal 2 [r bitop not bitmap:native:bitop:not bitmap:native:bitop:a]
        assert_equal bitmap [r type bitmap:native:bitop:not]
        assert_equal [binary format H* 0fff] [r debug bitmap-raw bitmap:native:bitop:not]
    }

    test {BITOP NOT honors proto-max-bulk-len for native bitmap sources} {
        set limit 1048576
        set oldval [config_get_set proto-max-bulk-len [expr {$limit + 1}]]
        r config set bitmap-default-roaring yes
        r del bitop:not:native:limit bitop:not:native:too-big \
            bitop:not:native:out bitop:not:native:sentinel

        assert_equal 0 [r setbit bitop:not:native:limit [expr {$limit * 8 - 1}] 1]
        assert_equal 0 [r setbit bitop:not:native:too-big [expr {($limit + 1) * 8 - 1}] 1]
        assert_equal bitmap [r type bitop:not:native:limit]
        assert_equal bitmap [r type bitop:not:native:too-big]

        r config set proto-max-bulk-len $limit
        assert_equal $limit [r bitop not bitop:not:native:out bitop:not:native:limit]
        assert_equal bitmap [r type bitop:not:native:out]
        assert_equal 1 [r getbit bitop:not:native:out [expr {$limit * 8 - 2}]]
        assert_equal 0 [r getbit bitop:not:native:out [expr {$limit * 8 - 1}]]

        r set bitop:not:native:sentinel keep
        assert_error {*string exceeds maximum allowed size (proto-max-bulk-len)*} {
            r bitop not bitop:not:native:sentinel bitop:not:native:too-big
        }
        assert_equal string [r type bitop:not:native:sentinel]
        assert_equal keep [r get bitop:not:native:sentinel]
        assert_equal bitmap [r type bitop:not:native:too-big]

        r config set bitmap-default-roaring no
        r config set proto-max-bulk-len $oldval
        r del bitop:not:native:limit bitop:not:native:too-big \
            bitop:not:native:out bitop:not:native:sentinel
    }

    test {non-NOT native BITOP survives lowering proto-max-bulk-len} {
        set limit 1048576
        set oldval [config_get_set proto-max-bulk-len [expr {$limit + 1}]]
        set last_bit [expr {($limit + 1) * 8 - 1}]

        r config set bitmap-default-roaring yes
        assert_equal 0 [r setbit bitop:limit:native $last_bit 1]
        r config set bitmap-default-roaring no
        assert_equal 0 [r setbit bitop:limit:string $last_bit 1]

        r config set proto-max-bulk-len $limit
        assert_equal [expr {$limit + 1}] \
            [r bitop or bitop:limit:native:out bitop:limit:native]
        assert_equal [expr {$limit + 1}] \
            [r bitop or bitop:limit:string:out bitop:limit:string]
        assert_equal bitmap [r type bitop:limit:native:out]
        assert_equal string [r type bitop:limit:string:out]
        assert_equal 1 [r bitcount bitop:limit:native:out]

        r config set proto-max-bulk-len [expr {$limit + 1}]
        assert_equal [r get bitop:limit:string:out] \
            [r debug bitmap-raw bitop:limit:native:out]

        r config set proto-max-bulk-len $oldval
        r del bitop:limit:native bitop:limit:string \
            bitop:limit:native:out bitop:limit:string:out
    }

    test {BITOP native bitmap sources match string bitmap results for all operations} {
        set a {0 4 5 6 20}
        set b {1 5 6 21}
        set c {2 3 5 6 7 20}

        assert_native_bitop_matches_string and AND [list $a $b $c]
        assert_native_bitop_matches_string or OR [list $a $b $c]
        assert_native_bitop_matches_string xor XOR [list $a $b $c]
        assert_native_bitop_matches_string diff DIFF [list $a $b $c]
        assert_native_bitop_matches_string diff1 DIFF1 [list $a $b $c]
        assert_native_bitop_matches_string andor ANDOR [list $a $b $c]
        assert_native_bitop_matches_string one ONE [list $a $b $c]
        assert_native_bitop_matches_string not NOT [list $a]
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
            assert_native_bitop_bitset_case $name $op $sources $expected $missing_indexes $alias_index $dest_seed
        }
    }

    test {BITOP current operation syntax errors are preserved on native paths} {
        seed_native_bitmap bitmap:native:bitop:syntax:a {1}
        seed_native_bitmap bitmap:native:bitop:syntax:b {2}

        assert_error {ERR syntax error} {
            r bitop noop bitmap:native:bitop:syntax:dest bitmap:native:bitop:syntax:a bitmap:native:bitop:syntax:b
        }
        assert_error {ERR BITOP NOT*} {
            r bitop not bitmap:native:bitop:syntax:dest bitmap:native:bitop:syntax:a bitmap:native:bitop:syntax:b
        }
        assert_error {ERR BITOP DIFF*} {
            r bitop diff bitmap:native:bitop:syntax:dest bitmap:native:bitop:syntax:a
        }
        assert_error {ERR BITOP DIFF1*} {
            r bitop diff1 bitmap:native:bitop:syntax:dest bitmap:native:bitop:syntax:a
        }
        assert_error {ERR BITOP ANDOR*} {
            r bitop andor bitmap:native:bitop:syntax:dest bitmap:native:bitop:syntax:a
        }
    }

    test {BITOP handles native bitmap empty sources and destination aliasing} {
        seed_native_bitmap bitmap:native:bitop:empty {}
        assert_equal 0 [r bitop not bitmap:native:bitop:empty-not bitmap:native:bitop:empty]
        assert_equal 0 [r exists bitmap:native:bitop:empty-not]

        seed_string_bitmap bitmap:native:bitop:alias:string:dest {0 2 4 6}
        seed_string_bitmap bitmap:native:bitop:alias:string:other {2 6 8}
        seed_native_bitmap bitmap:native:bitop:alias:native:dest {0 2 4 6}
        seed_native_bitmap bitmap:native:bitop:alias:native:other {2 6 8}

        set string_reply [r bitop diff bitmap:native:bitop:alias:string:dest bitmap:native:bitop:alias:string:dest bitmap:native:bitop:alias:string:other]
        set native_reply [r bitop diff bitmap:native:bitop:alias:native:dest bitmap:native:bitop:alias:native:dest bitmap:native:bitop:alias:native:other]
        assert_equal $string_reply $native_reply
        assert_equal [r get bitmap:native:bitop:alias:string:dest] [r debug bitmap-raw bitmap:native:bitop:alias:native:dest]
        assert_equal bitmap [r type bitmap:native:bitop:alias:native:dest]
    }

    test {BITOP frees an aliased native destination without touching stale sources} {
        # Regression: the destination's old value is also a source here, and
        # both store branches dispose of it (the delete branch frees it
        # outright), so bitopCommand()'s cleanup loop must not dereference
        # the source objects afterwards.

        # Delete branch: all-empty native sources with an aliased destination.
        seed_native_bitmap bitmap:native:bitop:self:empty {}
        assert_equal 0 [r bitop and bitmap:native:bitop:self:empty bitmap:native:bitop:self:empty]
        assert_equal 0 [r exists bitmap:native:bitop:self:empty]

        # Store branch: a self-targeting OR keeps the same bits.
        seed_native_bitmap bitmap:native:bitop:self:or {0 3 70000}
        assert_equal 8751 [r bitop or bitmap:native:bitop:self:or bitmap:native:bitop:self:or]
        assert_equal bitmap [r type bitmap:native:bitop:self:or]
        assert_equal 3 [r bitcount bitmap:native:bitop:self:or]
        assert_equal {1 1 1} [list \
            [r getbit bitmap:native:bitop:self:or 0] \
            [r getbit bitmap:native:bitop:self:or 3] \
            [r getbit bitmap:native:bitop:self:or 70000]]
    }

    test {BITOP mixed native and string sources match string results for all operations} {
        set a [binary format H* f000ff]
        set b [binary format H* 0f0f]
        set c [binary format H* 33000080]
        set raws [list $a $b $c]

        foreach op {and or xor diff diff1 andor one} {
            assert_native_bitop_raws_match_string "mixed:$op" $op $raws {0 2}
        }
        assert_native_bitop_raws_match_string mixed:not not [list $a] {0}
    }

    test {BITOP mixed dense string chunks match string results} {
        set native [binary format H* [string repeat aa 8194]]
        set dense [binary format H* "[string repeat ff 8192]8001"]
        set sparse [binary format H* [string repeat 00 8194]]
        set sparse [string replace $sparse 0 0 [binary format H* 80]]
        set sparse [string replace $sparse 8191 8191 [binary format H* 01]]
        set sparse [string replace $sparse 8192 8192 [binary format H* 80]]
        set sparse [string replace $sparse 8193 8193 [binary format H* 01]]
        set raws [list $native $dense $sparse]

        foreach op {and or xor diff diff1 andor one} {
            assert_native_bitop_raws_match_string "mixed-dense:$op" \
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
            assert_native_bitop_raws_match_string "mixed-benchmark:$op" \
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
            assert_native_bitop_raws_match_string "mixed-avx512:$op" \
                $op $raws {0 2 4 6}
        }
    }

    test {BITOP mixed native source destination aliasing matches string results} {
        set a [binary format H* aa5500]
        set b [binary format H* 0ff0]
        set c [binary format H* 330000f0]
        set raws [list $a $b $c]

        foreach {op alias_index native_indexes} {
            and   0 {0 2}
            or    1 {1 2}
            xor   2 {0 2}
            diff  0 {0 2}
            diff1 1 {1 2}
            andor 2 {0 2}
            one   0 {0 2}
        } {
            assert_native_bitop_raws_match_string "alias:$op:$alias_index" \
                $op $raws $native_indexes $alias_index
        }

        foreach {op alias_index native_indexes} {
            and   0 {2}
            or    1 {0 2}
            xor   2 {0}
            diff  0 {2}
            diff1 1 {0 2}
            andor 2 {0}
            one   0 {2}
        } {
            assert_native_bitop_raws_match_string "alias-string:$op:$alias_index" \
                $op $raws $native_indexes $alias_index
        }

        assert_native_bitop_raws_match_string alias:not not [list $a] {0} 0
    }

    test {BITOP mixed native fuzz matches bitmap-default-roaring no strings} {
        foreach op {and or xor diff diff1 andor one} {
            set min_args 1
            if {$op eq "diff" || $op eq "diff1" || $op eq "andor"} {
                set min_args 2
            }

            for {set i 0} {$i < 12} {incr i} {
                set raws {}
                set native_indexes {}
                set count [expr {$min_args + [randomInt 4]}]

                for {set j 0} {$j < $count} {incr j} {
                    lappend raws [randstring 0 128]
                    if {[expr {($i + $j) % 2}] == 0} {
                        lappend native_indexes $j
                    }
                }

                assert_native_bitop_raws_match_string "fuzz:$op:$i" \
                    $op $raws $native_indexes
            }
        }

        for {set i 0} {$i < 12} {incr i} {
            assert_native_bitop_raws_match_string "fuzz:not:$i" \
                not [list [randstring 0 128]] {0}
        }
    }

    test {BITOP mixed native and missing-key sources match string results} {
        r config set bitmap-default-roaring no

        set a [binary format H* f0f0]
        set c [binary format H* 0f]

        foreach op {and or xor diff diff1 andor one} {
            r del bitop:miss:string:dest bitop:miss:native:dest
            r del bitop:miss:string:a bitop:miss:string:gone bitop:miss:string:c
            r del bitop:miss:native:a bitop:miss:native:gone bitop:miss:native:c

            r set bitop:miss:string:a $a
            r set bitop:miss:string:c $c
            r set bitop:miss:native:a $a
            r set bitop:miss:native:c $c
            convert_string_bitmap_to_native r bitop:miss:native:a
            set string_reply [r bitop $op bitop:miss:string:dest \
                bitop:miss:string:a bitop:miss:string:gone bitop:miss:string:c]
            set native_reply [r bitop $op bitop:miss:native:dest \
                bitop:miss:native:a bitop:miss:native:gone bitop:miss:native:c]
            assert_equal $string_reply $native_reply
            assert_equal [bitmap_logical_raw bitop:miss:string:dest] \
                [bitmap_logical_raw bitop:miss:native:dest]
        }
    }

    test {BITOP with a missing first source matches string results on the native path} {
        # The empty-accumulator seeding branches (sources[0] == NULL) are
        # distinct code paths: AND/ANDOR clear the result, DIFF1 skips the
        # andnot, and the generic copy falls back to an empty roaring.
        r config set bitmap-default-roaring no

        set a [binary format H* f0f0]
        set c [binary format H* 0f]

        foreach op {and or xor diff diff1 andor one} {
            r del bitop:first:string:dest bitop:first:native:dest
            r del bitop:first:string:gone bitop:first:string:a bitop:first:string:c
            r del bitop:first:native:gone bitop:first:native:a bitop:first:native:c

            r set bitop:first:string:a $a
            r set bitop:first:string:c $c
            r set bitop:first:native:a $a
            r set bitop:first:native:c $c
            convert_string_bitmap_to_native r bitop:first:native:a
            set string_reply [r bitop $op bitop:first:string:dest \
                bitop:first:string:gone bitop:first:string:a bitop:first:string:c]
            set native_reply [r bitop $op bitop:first:native:dest \
                bitop:first:native:gone bitop:first:native:a bitop:first:native:c]
            assert_equal $string_reply $native_reply
            assert_equal [bitmap_logical_raw bitop:first:string:dest] \
                [bitmap_logical_raw bitop:first:native:dest]
        }
    }

    test {BITOP duplicate sources match string results on the native path} {
        r config set bitmap-default-roaring no

        set a [binary format H* aa5500]
        set s [binary format H* 0ff0]

        # The same native bitmap key twice: both slots borrow the same
        # roaring, so the accumulator must deep-copy rather than steal.
        foreach op {and or xor diff diff1 andor one} {
            r del bitop:dup:string:dest bitop:dup:native:dest
            r del bitop:dup:string:k bitop:dup:native:k
            r set bitop:dup:string:k $a
            r set bitop:dup:native:k $a
            convert_string_bitmap_to_native r bitop:dup:native:k
            set string_reply [r bitop $op bitop:dup:string:dest \
                bitop:dup:string:k bitop:dup:string:k]
            set native_reply [r bitop $op bitop:dup:native:dest \
                bitop:dup:native:k bitop:dup:native:k]
            assert_equal $string_reply $native_reply
            assert_equal [bitmap_logical_raw bitop:dup:string:dest] \
                [bitmap_logical_raw bitop:dup:native:dest]
        }

        # The same string key twice alongside a native source: each slot
        # builds an independent owned roaring, so the slot-0 steal cannot
        # affect the second operand.
        foreach op {and or xor diff diff1 andor one} {
            r del bitop:dup2:string:dest bitop:dup2:native:dest
            r del bitop:dup2:string:s bitop:dup2:native:s
            r del bitop:dup2:string:n bitop:dup2:native:n
            r set bitop:dup2:string:s $s
            r set bitop:dup2:native:s $s
            r set bitop:dup2:string:n $a
            r set bitop:dup2:native:n $a
            convert_string_bitmap_to_native r bitop:dup2:native:n
            set string_reply [r bitop $op bitop:dup2:string:dest \
                bitop:dup2:string:s bitop:dup2:string:s bitop:dup2:string:n]
            set native_reply [r bitop $op bitop:dup2:native:dest \
                bitop:dup2:native:s bitop:dup2:native:s bitop:dup2:native:n]
            assert_equal $string_reply $native_reply
            assert_equal [bitmap_logical_raw bitop:dup2:string:dest] \
                [bitmap_logical_raw bitop:dup2:native:dest]
        }
    }

    test {BITOP rejects non-string non-bitmap sources mixed with native bitmaps} {
        seed_native_bitmap bitop:wrongtype:native {0 9}
        r del bitop:wrongtype:list bitop:wrongtype:dest
        r rpush bitop:wrongtype:list element

        # The type error fires after earlier sources may already be prepared,
        # exercising the cleanup of converted operands under sanitizer runs.
        assert_error {WRONGTYPE*} {
            r bitop and bitop:wrongtype:dest bitop:wrongtype:native bitop:wrongtype:list
        }
        assert_error {WRONGTYPE*} {
            r bitop xor bitop:wrongtype:dest bitop:wrongtype:list bitop:wrongtype:native
        }
        assert_equal 0 [r exists bitop:wrongtype:dest]
        assert_equal bitmap [r type bitop:wrongtype:native]
        assert_equal bitmap-roaring [r object encoding bitop:wrongtype:native]
    }
}


start_server {tags {"bitmap" "bitmap-native" "cluster:skip"}} {
    test {native bitmap BITOP supports OLAP columnar index user stories} {
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
            seed_native_bitmap "bitmap:olap:$index" $bits
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

    test {native bitmap BITOP models Pinot inverted index examples} {
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
            seed_native_bitmap "bitmap:pinot:$index" $bits
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


start_server {tags {"bitmap" "bitmap-native" "cluster:skip"}} {
    test {native bitmap BITOP models Druid Wikipedia query tutorial filters} {
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
            seed_native_bitmap "bitmap:wikipedia:$index" $bits
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

start_server {tags {"bitmap" "bitmap-native" "needs:debug" "needs:save" "cluster:skip"}} {
    test {native bitmap RDB save and reload survive lowering proto-max-bulk-len} {
        r config set bitmap-default-roaring yes
        r del bitmap:proto:shrink
        r setbit bitmap:proto:shrink 16777215 1
        r config set bitmap-default-roaring no
        assert_equal bitmap [r type bitmap:proto:shrink]

        # The RDB writer materializes logical bytes internally and must not be
        # bounded by the client protocol limit.
        set old [config_get_set proto-max-bulk-len 1048576]
        r debug reload
        assert_equal bitmap [r type bitmap:proto:shrink]
        assert_equal bitmap-roaring [r object encoding bitmap:proto:shrink]
        assert_equal 1 [r bitcount bitmap:proto:shrink]

        r config set proto-max-bulk-len $old
        assert_equal 1 [r getbit bitmap:proto:shrink 16777215]
        r del bitmap:proto:shrink
    }

    test {native bitmap RDB round-trip with rdbcompression no} {
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

    test {redis-check-rdb validates dumps containing native bitmaps} {
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
