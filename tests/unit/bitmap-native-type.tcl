set testmodule [file normalize tests/modules/misc.so]
set bitmapnotifymodule [file normalize tests/modules/bitmap_notify.so]
set ::sparse_public_offset 65536
set ::sparse_public_len 8193

start_server {tags {"bitmap" "bitmap-native" "needs:debug" "cluster:skip"}} {
    test {bitmap-default-roaring defaults to no} {
        assert_equal no [lindex [r config get bitmap-default-roaring] 1]
    }

    test {BITMAP HELP documents the observable bitmap type split} {
        set help [join [r bitmap help] "\n"]

        assert_match {*NATIVE changes TYPE to bitmap*} $help
        assert_match {*STRING*changes TYPE back to string*} $help
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

        r set bitmap:public:zero:existing ""
        r bitmap convert bitmap:public:zero:existing
        assert_equal "" [r debug bitmap-raw bitmap:public:zero:existing]
        set dirty [s rdb_changes_since_last_save]
        assert_equal 0 [r setbit bitmap:public:zero:existing 0 0]
        assert_equal bitmap [r type bitmap:public:zero:existing]
        assert_equal [binary format H* 00] [r debug bitmap-raw bitmap:public:zero:existing]
        assert_equal [expr {$dirty + 1}] [s rdb_changes_since_last_save]

        set dirty [s rdb_changes_since_last_save]
        assert_equal 0 [r setbit bitmap:public:zero:existing 0 0]
        assert_equal [binary format H* 00] [r debug bitmap-raw bitmap:public:zero:existing]
        assert_equal $dirty [s rdb_changes_since_last_save]
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

    test {BITMAP CONVERT converts strings to native bitmaps and back} {
        r config set bitmap-default-roaring no
        set raw [binary format H* 80400100080000]

        r del bitmap:convert
        r set bitmap:convert $raw
        r pexpire bitmap:convert 60000
        assert_equal OK [r bitmap convert bitmap:convert]
        assert_equal bitmap [r type bitmap:convert]
        assert_equal bitmap-roaring [r object encoding bitmap:convert]
        assert_equal $raw [r debug bitmap-raw bitmap:convert]
        assert {[r pttl bitmap:convert] > 0}

        # Idempotent in both directions.
        assert_equal OK [r bitmap convert bitmap:convert]
        assert_equal OK [r bitmap convert bitmap:convert NATIVE]
        assert_equal bitmap [r type bitmap:convert]

        assert_equal OK [r bitmap convert bitmap:convert STRING]
        assert_equal string [r type bitmap:convert]
        assert_equal $raw [r get bitmap:convert]
        assert {[r pttl bitmap:convert] > 0}
        assert_equal OK [r bitmap convert bitmap:convert STRING]
        assert_equal string [r type bitmap:convert]
    }

    test {BITMAP CONVERT preserves all-zero logical byte length} {
        r config set bitmap-default-roaring no
        set raw [string repeat [binary format H* 00] 6]

        r del bitmap:convert:zeros bitmap:convert:zeros:restored
        r set bitmap:convert:zeros $raw
        assert_equal OK [r bitmap convert bitmap:convert:zeros]
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

    test {BITMAP CONVERT handles int-encoded strings missing keys and wrong types} {
        r del bitmap:convert:int bitmap:convert:list
        r set bitmap:convert:int 12345
        assert_equal int [r object encoding bitmap:convert:int]
        assert_equal OK [r bitmap convert bitmap:convert:int]
        assert_equal bitmap [r type bitmap:convert:int]
        assert_equal "12345" [r debug bitmap-raw bitmap:convert:int]

        assert_error {ERR no such key} {r bitmap convert bitmap:convert:missing}
        r rpush bitmap:convert:list element
        assert_error {WRONGTYPE*} {r bitmap convert bitmap:convert:list}
        assert_error {*syntax*} {r bitmap convert bitmap:convert:int SIDEWAYS}
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
        assert_equal OK [r bitmap convert bitmap:digest:converted]

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

    test {native bitmaps keep the original proto-max-bulk-len offset limit} {
        r del bitmap:native:bounds
        r config set bitmap-default-roaring yes
        assert_equal 0 [r setbit bitmap:native:bounds 0 1]
        r config set bitmap-default-roaring no

        assert_equal bitmap [r type bitmap:native:bounds]
        assert_equal 1 [r bitcount bitmap:native:bounds]
        foreach cmd {
            {getbit bitmap:native:bounds 4294967296}
            {setbit bitmap:native:bounds 4294967296 1}
            {bitfield_ro bitmap:native:bounds GET u1 4294967296}
            {bitfield bitmap:native:bounds SET u1 4294967296 1}
        } {
            assert_error {*bit offset*out of range*} {r {*}$cmd}
        }
        assert_error {*bit offset is*out of range*} {
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
            [list setbit bitmap:native:small-limit $first_rejected 1] \
            [list bitfield_ro bitmap:native:small-limit GET u1 $first_rejected] \
            [list bitfield bitmap:native:small-limit SET u1 $first_rejected 1] \
        ] {
            assert_error {*bit offset*out of range*} {r {*}$cmd}
        }
        assert_error {*bit offset*out of range*} {
            r bitfield bitmap:native:small-limit SET u2 $last_allowed 3
        }
        assert_equal 1 [r bitcount bitmap:native:small-limit]

        r config set bitmap-default-roaring no
        r config set proto-max-bulk-len $oldval
        r del bitmap:native:small-limit
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
        r del bitmap:public:notify bitmap:public:notify:conv bitmap:public:notify:cmd

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

        # Explicit conversion emits the overwrite pair only.
        r set bitmap:public:notify:cmd ""
        assert_equal {pmessage __keyevent@9__:* __keyevent@9__:new bitmap:public:notify:cmd} [$rd read]
        assert_equal {pmessage __keyevent@9__:* __keyevent@9__:set bitmap:public:notify:cmd} [$rd read]
        r bitmap convert bitmap:public:notify:cmd
        assert_equal {pmessage __keyevent@9__:* __keyevent@9__:overwritten bitmap:public:notify:cmd} [$rd read]
        assert_equal {pmessage __keyevent@9__:* __keyevent@9__:type_changed bitmap:public:notify:cmd} [$rd read]

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
        r bitmap convert bitop:dest:n1
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
        assert_equal [r bitmap convert bitmap:raw] OK
        assert_equal [r type bitmap:raw] bitmap
        assert_equal [r object encoding bitmap:raw] bitmap-roaring
        assert_equal [r debug bitmap-raw bitmap:raw] $raw
        assert_error {WRONGTYPE*} {r get bitmap:raw}
    }

    test {native bitmap scan type and copy preserve bitmap objects} {
        set raw [binary format H* 010204000000]

        r set bitmap:copy-source $raw
        r set bitmap:string-peer value
        r bitmap convert bitmap:copy-source

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
        r bitmap convert bitmap:string-boundary

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
        r bitmap convert bitmap:set-overwrite
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
        r bitmap convert bitmap:nx-boundary

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
        r bitmap convert bitmap:surface

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
        r bitmap convert weight_a
        r bitmap convert data_a
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
        r bitmap convert wh_a
        assert_equal {a b} [r sort bitmap:sort:list BY wh_*->f GET #]
        assert_equal [list {} 1] [r sort bitmap:sort:list BY wh_*->f GET wh_*->f]
        assert_equal bitmap [r type wh_a]
    }

    test {Lua scripts observe native bitmaps through normal type checks} {
        set raw [binary format H* 80400100080000]

        r set bitmap:lua $raw
        r bitmap convert bitmap:lua

        assert_equal 1 [r eval {return redis.call('getbit', KEYS[1], 0)} 1 bitmap:lua]
        assert_equal 4 [r eval {return redis.call('bitcount', KEYS[1])} 1 bitmap:lua]
        assert_error {*WRONGTYPE*} {r eval {return redis.call('get', KEYS[1])} 1 bitmap:lua}
        assert_equal bitmap [r type bitmap:lua]
    }

    test {native bitmap dump restore and debug reload preserve bitmap objects} {
        set raw [binary format H* f0000000000000010000]

        r set bitmap:persist $raw
        r bitmap convert bitmap:persist

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

        r bitmap convert bitmap:restore:source
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

    test {native bitmap RDB restores run containers without capacity bloat} {
        set raw ""
        for {set i 0} {$i < 32} {incr i} {
            append raw [string repeat [binary format H* ff] 600]
            append raw [string repeat [binary format H* 00] 7592]
        }

        r del bitmap:rdb-run:a bitmap:rdb-run:b
        r set bitmap:rdb-run:a $raw
        r bitmap convert bitmap:rdb-run:a
        set original_usage [r memory usage bitmap:rdb-run:a]

        r restore bitmap:rdb-run:b 0 [r dump bitmap:rdb-run:a]
        assert_equal bitmap [r type bitmap:rdb-run:b]
        assert_equal $raw [r debug bitmap-raw bitmap:rdb-run:b]

        set restored_usage [r memory usage bitmap:rdb-run:b]
        assert_lessthan_equal $restored_usage [expr {$original_usage + 8192}] \
            "restored_usage=$restored_usage original_usage=$original_usage"
        r del bitmap:rdb-run:a bitmap:rdb-run:b
    }

    test {native bitmap RDB uses raw payload for fragmented bitmaps} {
        set raw [string repeat [binary format H* 55] 8192]

        r del bitmap:rdb-frag:a bitmap:rdb-frag:b
        r set bitmap:rdb-frag:a $raw
        r bitmap convert bitmap:rdb-frag:a

        set dump [r dump bitmap:rdb-frag:a]
        assert_lessthan [string length $dump] [expr {[string length $raw] + 128}] \
            "dump_len=[string length $dump] raw_len=[string length $raw]"

        r restore bitmap:rdb-frag:b 0 $dump
        assert_equal bitmap [r type bitmap:rdb-frag:b]
        assert_equal $raw [r debug bitmap-raw bitmap:rdb-frag:b]
        r del bitmap:rdb-frag:a bitmap:rdb-frag:b
    }

    test {native bitmap RDB raw save is not bounded by current proto-max-bulk-len} {
        set limit 1048576
        set byte_len [expr {$limit + 1}]
        set oldval [config_get_set proto-max-bulk-len [expr {$byte_len + 1024}]]
        set raw [string repeat [binary format H* 8000] [expr {$limit / 2}]]
        append raw [binary format H* 80]
        assert_equal $byte_len [string length $raw]

        r del bitmap:rdb-raw-bulk-limit
        r set bitmap:rdb-raw-bulk-limit $raw
        r bitmap convert bitmap:rdb-raw-bulk-limit
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
        r bitmap convert bitmap:module-boundary

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
        r bitmap convert bitmap:aof
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

    test {AOF rewrite preserves deterministic bitmap and string transitions} {
        r flushall
        r config set appendonly yes
        waitForBgrewriteaof r
        r config set auto-aof-rewrite-percentage 0

        set raw [binary format H* 80400100080000]
        r set bitmap:aof:transition:native $raw
        r bitmap convert bitmap:aof:transition:native

        r set bitmap:aof:transition:string $raw
        r bitmap convert bitmap:aof:transition:string
        r bitmap convert bitmap:aof:transition:string STRING

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

        test {BITMAP CONVERT replicates the conversion as RESTORE} {
            set raw [binary format H* 80400100080000]

            $master config set bitmap-default-roaring no
            $master set bitmap:public:repl:conv $raw
            wait_for_ofs_sync $master $replica
            assert_equal string [$replica type bitmap:public:repl:conv]

            # The conversion must arrive as the RESTORE effect, never as a
            # replayed BITMAP CONVERT: whether the replica could re-run the
            # conversion depends on its own configuration.
            $master bitmap convert bitmap:public:repl:conv
            wait_for_ofs_sync $master $replica

            assert_equal bitmap [$replica type bitmap:public:repl:conv]
            assert_equal bitmap-roaring [$replica object encoding bitmap:public:repl:conv]
            assert_equal $raw [$replica debug bitmap-raw bitmap:public:repl:conv]
            assert_equal [$master debug digest] [$replica debug digest]

            # And back: the string conversion also replicates as its effect.
            $master bitmap convert bitmap:public:repl:conv STRING
            wait_for_ofs_sync $master $replica
            assert_equal string [$replica type bitmap:public:repl:conv]
            assert_equal $raw [$replica get bitmap:public:repl:conv]
            assert_equal [$master debug digest] [$replica debug digest]
        }

        test {RESTORE payloads replicate bitmap and string type transitions} {
            set raw [binary format H* 80400100080000]

            $master del bitmap:public:repl:restore:source bitmap:public:repl:restore:target
            $master set bitmap:public:repl:restore:source $raw
            set string_payload [$master dump bitmap:public:repl:restore:source]
            $master bitmap convert bitmap:public:repl:restore:source
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

start_server {tags {"bitmap" "bitmap-native" "repl" "modules" "external:skip" "cluster:skip"}} {
    start_server {} {
        set master [srv -1 client]
        set master_host [srv -1 host]
        set master_port [srv -1 port]
        set replica [srv 0 client]

        $master module load $bitmapnotifymodule
        $replica replicaof $master_host $master_port
        wait_for_sync $replica
        wait_for_ofs_sync $master $replica

        proc assert_bitmap_notify_no_key {master replica key} {
            wait_for_ofs_sync $master $replica
            assert_equal 1 [$master bitmapnotify.hits]
            assert_equal 0 [$master exists $key]
            assert_equal 0 [$replica exists $key]
            assert_equal [$master debug digest] [$replica debug digest]
        }

        proc assert_bitmap_notify_string {master replica key value} {
            wait_for_ofs_sync $master $replica
            assert_equal 1 [$master bitmapnotify.hits]
            assert_equal string [$master type $key]
            assert_equal string [$replica type $key]
            assert_equal $value [$master get $key]
            assert_equal $value [$replica get $key]
            assert_equal [$master debug digest] [$replica debug digest]
        }

        proc arm_bitmap_notify {master key event action args} {
            assert_equal OK [$master bitmapnotify.clear]
            assert_equal OK [$master bitmapnotify.arm $key $event $action {*}$args]
        }

        $master config set bitmap-default-roaring yes
        $replica config set bitmap-default-roaring no

        test {SETBIT native creation queues RESTORE before new notification mutation} {
            set key bitmap:notify-race:setbit-new
            $master del $key
            wait_for_ofs_sync $master $replica

            arm_bitmap_notify $master $key new del
            assert_equal 0 [$master setbit $key $::sparse_public_offset 1]

            assert_bitmap_notify_no_key $master $replica $key
        }

        test {SETBIT native conversion queues RESTORE before overwritten notification mutation} {
            set key bitmap:notify-race:setbit-overwritten
            set value module-overwrote-setbit
            $master set $key ""
            wait_for_ofs_sync $master $replica

            arm_bitmap_notify $master $key overwritten set $value
            assert_equal 0 [$master setbit $key $::sparse_public_offset 1]

            assert_bitmap_notify_string $master $replica $key $value
        }

        test {BITFIELD native creation queues RESTORE before new notification mutation} {
            set key bitmap:notify-race:bitfield-new
            set value module-overwrote-bitfield-new
            $master del $key
            wait_for_ofs_sync $master $replica

            arm_bitmap_notify $master $key new set $value
            assert_equal {0} [$master bitfield $key SET u1 $::sparse_public_offset 1]

            assert_bitmap_notify_string $master $replica $key $value
        }

        test {BITFIELD native conversion queues RESTORE before type_changed notification mutation} {
            set key bitmap:notify-race:bitfield-type
            $master set $key ""
            wait_for_ofs_sync $master $replica

            arm_bitmap_notify $master $key type_changed del
            assert_equal {0} [$master bitfield $key SET u1 $::sparse_public_offset 1]

            assert_bitmap_notify_no_key $master $replica $key
        }

        test {BITOP native destination queues RESTORE before type_changed notification mutation} {
            set key bitmap:notify-race:bitop-type
            set value module-overwrote-bitop
            $master set bitmap:notify-race:bitop-src1 [binary format H* f0]
            $master set bitmap:notify-race:bitop-src2 [binary format H* 0f]
            $master set $key old
            wait_for_ofs_sync $master $replica

            arm_bitmap_notify $master $key type_changed set $value
            assert_equal 1 [$master bitop or $key bitmap:notify-race:bitop-src1 bitmap:notify-race:bitop-src2]

            assert_bitmap_notify_string $master $replica $key $value
        }

        test {BITMAP CONVERT queues RESTORE before type_changed notification mutation} {
            set key bitmap:notify-race:convert-type
            set raw [binary format H* 80400100080000]
            $master set $key $raw
            wait_for_ofs_sync $master $replica

            arm_bitmap_notify $master $key type_changed del
            assert_equal OK [$master bitmap convert $key]

            assert_bitmap_notify_no_key $master $replica $key
        }
    }
}
