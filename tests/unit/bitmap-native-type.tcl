set testmodule [file normalize tests/modules/misc.so]
set ::sparse_public_offset 65536
set ::sparse_public_len 8193

proc configure_native_bitmap_creation {{auto no} {minbytes 1} {minsaving 0}} {
    r config set bitmap-roaring-enabled yes
    r config set bitmap-roaring-auto-convert $auto
    r config set bitmap-roaring-min-bytes $minbytes
    r config set bitmap-roaring-min-saving $minsaving
}

start_server {tags {"bitmap" "bitmap-native" "needs:debug" "cluster:skip"}} {
    test {native bitmap creation configs default to opt-in behavior} {
        assert_equal no [lindex [r config get bitmap-roaring-enabled] 1]
        assert_equal no [lindex [r config get bitmap-roaring-auto-convert] 1]
    }

    test {SETBIT keeps creating strings when native bitmap creation is disabled} {
        configure_native_bitmap_creation no 1 0
        r config set bitmap-roaring-enabled no

        assert_equal 0 [r setbit bitmap:public:disabled $::sparse_public_offset 1]
        assert_equal string [r type bitmap:public:disabled]
        assert_equal $::sparse_public_len [r strlen bitmap:public:disabled]
        assert_equal 1 [r getbit bitmap:public:disabled $::sparse_public_offset]
    }

    test {SETBIT creates native bitmap for eligible missing sparse keys} {
        configure_native_bitmap_creation no 1 0

        assert_equal 0 [r setbit bitmap:public:create $::sparse_public_offset 1]
        assert_equal bitmap [r type bitmap:public:create]
        assert_equal bitmap-roaring [r object encoding bitmap:public:create]
        assert_equal 1 [r getbit bitmap:public:create $::sparse_public_offset]
        assert_equal 1 [r bitcount bitmap:public:create]
        assert_equal $::sparse_public_len [string length [r debug bitmap-raw bitmap:public:create]]
        assert_error {WRONGTYPE*} {r get bitmap:public:create}
    }

    test {native bitmap creation respects byte and saving thresholds} {
        r del bitmap:public:min-bytes bitmap:public:min-saving

        configure_native_bitmap_creation no 100 0
        assert_equal 0 [r setbit bitmap:public:min-bytes 80 1]
        assert_equal string [r type bitmap:public:min-bytes]
        assert_equal 11 [r strlen bitmap:public:min-bytes]

        configure_native_bitmap_creation no 1 100000
        assert_equal 0 [r setbit bitmap:public:min-saving $::sparse_public_offset 1]
        assert_equal string [r type bitmap:public:min-saving]
        assert_equal $::sparse_public_len [r strlen bitmap:public:min-saving]
    }

    test {auto-convert defaults off for existing string bitmaps} {
        configure_native_bitmap_creation no 1 0

        r set bitmap:public:auto-off ""
        assert_equal 0 [r setbit bitmap:public:auto-off $::sparse_public_offset 1]
        assert_equal string [r type bitmap:public:auto-off]
        assert_equal $::sparse_public_len [r strlen bitmap:public:auto-off]
    }

    test {auto-convert opt-in converts growing string bitmaps and keeps TTL} {
        configure_native_bitmap_creation yes 1 0

        r set bitmap:public:auto-on ""
        r pexpire bitmap:public:auto-on 60000
        assert_equal 0 [r setbit bitmap:public:auto-on $::sparse_public_offset 1]
        assert_equal bitmap [r type bitmap:public:auto-on]
        assert_equal bitmap-roaring [r object encoding bitmap:public:auto-on]
        assert_equal 1 [r getbit bitmap:public:auto-on $::sparse_public_offset]
        assert {[r pttl bitmap:public:auto-on] > 0}
    }

    test {public native bitmaps cover the bitmap command surface} {
        configure_native_bitmap_creation no 1 0

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
        assert_equal string [r type bitmap:public:commands:copy]
    }

    test {native bitmap helper exposes type encoding and exact raw bytes} {
        set raw [binary format H* 80400100080000]

        r set bitmap:raw $raw
        assert_equal [r debug bitmap-force-roaring bitmap:raw] OK
        assert_equal [r type bitmap:raw] bitmap
        assert_equal [r object encoding bitmap:raw] bitmap-roaring
        assert_equal [r debug bitmap-raw bitmap:raw] $raw
        assert_error {WRONGTYPE*} {r get bitmap:raw}
    }

    test {native bitmap scan type and copy preserve bitmap objects} {
        set raw [binary format H* 010204000000]

        r set bitmap:copy-source $raw
        r set bitmap:string-peer value
        r debug bitmap-force-roaring bitmap:copy-source

        set scan_reply [r scan 0 type bitmap]
        set keys [lindex $scan_reply 1]
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
        r debug bitmap-force-roaring bitmap:string-boundary

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
        r debug bitmap-force-roaring bitmap:set-overwrite
        assert_equal bitmap [r type bitmap:set-overwrite]

        # Generic overwrite is the intended plain replacement path: SET
        # replaces a native bitmap like it replaces any other type, while
        # implicit string reads stay WRONGTYPE.
        r set bitmap:set-overwrite replacement
        assert_equal string [r type bitmap:set-overwrite]
        assert_equal replacement [r get bitmap:set-overwrite]
    }

    test {native bitmap stays opaque to additional string read surfaces} {
        set raw [binary format H* 80400100080000]

        r set bitmap:surface $raw
        r set bitmap:surface:string $raw
        r debug bitmap-force-roaring bitmap:surface

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
        r debug bitmap-force-roaring weight_a
        r debug bitmap-force-roaring data_a
        assert_equal {a b} [r sort bitmap:sort:list BY weight_* GET #]
        assert_equal [list {} string-b] [r sort bitmap:sort:list BY weight_* GET data_*]
        assert_equal bitmap [r type weight_a]
        assert_equal bitmap [r type data_a]
    }

    test {Lua scripts observe native bitmaps through normal type checks} {
        set raw [binary format H* 80400100080000]

        r set bitmap:lua $raw
        r debug bitmap-force-roaring bitmap:lua

        assert_equal 1 [r eval {return redis.call('getbit', KEYS[1], 0)} 1 bitmap:lua]
        assert_equal 4 [r eval {return redis.call('bitcount', KEYS[1])} 1 bitmap:lua]
        assert_error {*WRONGTYPE*} {r eval {return redis.call('get', KEYS[1])} 1 bitmap:lua}
        assert_equal bitmap [r type bitmap:lua]
    }

    test {native bitmap dump restore and debug reload preserve bitmap objects} {
        set raw [binary format H* f0000000000000010000]

        r set bitmap:persist $raw
        r debug bitmap-force-roaring bitmap:persist

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

    test {public-created native bitmaps survive debug reload} {
        configure_native_bitmap_creation yes 1 0

        r setbit bitmap:public:reload:direct $::sparse_public_offset 1
        r set bitmap:public:reload:auto ""
        r setbit bitmap:public:reload:auto $::sparse_public_offset 1
        set digest_before [debug_digest]

        r debug reload

        assert_equal [debug_digest] $digest_before
        assert_equal bitmap [r type bitmap:public:reload:direct]
        assert_equal bitmap [r type bitmap:public:reload:auto]
        assert_equal 1 [r getbit bitmap:public:reload:direct $::sparse_public_offset]
        assert_equal 1 [r getbit bitmap:public:reload:auto $::sparse_public_offset]
    }
}

start_server {tags {"bitmap" "bitmap-native" "needs:debug" "modules" "external:skip" "cluster:skip"}} {
    r module load $testmodule

    test {module key API exposes bitmap without string access} {
        set raw [binary format H* 80400100080000]

        r set bitmap:module-boundary $raw
        r set bitmap:module-string $raw
        r debug bitmap-force-roaring bitmap:module-boundary

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
        r debug bitmap-force-roaring bitmap:aof
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
        r config set auto-aof-rewrite-percentage 0
        configure_native_bitmap_creation yes 1 0

        r setbit bitmap:public:aof:direct $::sparse_public_offset 1
        r set bitmap:public:aof:auto ""
        r setbit bitmap:public:aof:auto $::sparse_public_offset 1
        set digest_before [debug_digest]

        r bgrewriteaof
        waitForBgrewriteaof r
        r debug loadaof

        assert_equal [debug_digest] $digest_before
        assert_equal bitmap [r type bitmap:public:aof:direct]
        assert_equal bitmap [r type bitmap:public:aof:auto]
        assert_equal 1 [r getbit bitmap:public:aof:direct $::sparse_public_offset]
        assert_equal 1 [r getbit bitmap:public:aof:auto $::sparse_public_offset]
    }
}

start_server {tags {"bitmap" "bitmap-native" "repl" "external:skip" "cluster:skip"}} {
    start_server {} {
        set master [srv -1 client]
        set master_host [srv -1 host]
        set master_port [srv -1 port]
        set replica [srv 0 client]

        test {native bitmap public creation replicates deterministic type transitions} {
            $replica replicaof $master_host $master_port
            wait_for_condition 50 100 {
                [s 0 master_link_status] eq {up}
            } else {
                fail "Replication not started."
            }

            $master config set bitmap-roaring-enabled yes
            $master config set bitmap-roaring-auto-convert yes
            $master config set bitmap-roaring-min-bytes 1
            $master config set bitmap-roaring-min-saving 0
            $replica config set bitmap-roaring-enabled no
            $replica config set bitmap-roaring-auto-convert no

            $master setbit bitmap:public:repl:direct $::sparse_public_offset 1
            $master set bitmap:public:repl:auto ""
            $master setbit bitmap:public:repl:auto $::sparse_public_offset 1
            wait_for_ofs_sync $master $replica

            assert_equal bitmap [$replica type bitmap:public:repl:direct]
            assert_equal bitmap [$replica type bitmap:public:repl:auto]
            assert_equal 1 [$replica getbit bitmap:public:repl:direct $::sparse_public_offset]
            assert_equal 1 [$replica getbit bitmap:public:repl:auto $::sparse_public_offset]
            assert_error {WRONGTYPE*} {$replica get bitmap:public:repl:direct}
            assert_error {WRONGTYPE*} {$replica get bitmap:public:repl:auto}
        }
    }
}
