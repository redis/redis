start_server {tags {"bitmap" "bitmap-native" "needs:debug" "cluster:skip"}} {
    test {native bitmap read commands preserve type encoding and bytes} {
        set raw [binary format H* 80400100080000]

        r set bitmap:native:read $raw
        r debug bitmap-force-roaring bitmap:native:read

        assert_equal 1 [r getbit bitmap:native:read 0]
        assert_equal 1 [r getbit bitmap:native:read 9]
        assert_equal 0 [r getbit bitmap:native:read 10]
        assert_equal 4 [r bitcount bitmap:native:read]
        assert_equal 2 [r bitcount bitmap:native:read 8 23 bit]
        assert_equal 0 [r bitpos bitmap:native:read 1]
        assert_equal 1 [r bitpos bitmap:native:read 0]
        assert_equal 9 [r bitpos bitmap:native:read 1 8 -1 bit]
        assert_equal {1 1 1} [r bitfield_ro bitmap:native:read GET u1 0 GET u1 9 GET u1 36]

        assert_equal bitmap [r type bitmap:native:read]
        assert_equal bitmap-roaring [r object encoding bitmap:native:read]
        assert_equal $raw [r debug bitmap-raw bitmap:native:read]
    }

    test {SETBIT updates native bitmap values and preserves trailing zero length} {
        r set bitmap:native:setbit [binary format H* 8000]
        r debug bitmap-force-roaring bitmap:native:setbit

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

    test {BITFIELD writes native bitmap values through materialization fallback} {
        r set bitmap:native:bitfield [binary format H* 00]
        r debug bitmap-force-roaring bitmap:native:bitfield

        assert_equal {0 15} [r bitfield bitmap:native:bitfield SET u4 4 15 GET u8 0]
        assert_equal bitmap [r type bitmap:native:bitfield]
        assert_equal bitmap-roaring [r object encoding bitmap:native:bitfield]
        assert_equal [binary format H* 0f] [r debug bitmap-raw bitmap:native:bitfield]
        assert_equal {15} [r bitfield_ro bitmap:native:bitfield GET u4 4]

        assert_equal {0} [r bitfield bitmap:native:bitfield SET u1 23 0]
        assert_equal bitmap [r type bitmap:native:bitfield]
        assert_equal [binary format H* 0f0000] [r debug bitmap-raw bitmap:native:bitfield]
    }

    test {BITOP accepts native bitmap sources and stores string destinations} {
        r set bitmap:native:bitop:a [binary format H* f000]
        r debug bitmap-force-roaring bitmap:native:bitop:a
        r set bitmap:native:bitop:b [binary format H* 0fff]
        r set bitmap:native:bitop:dest [binary format H* aa]
        r debug bitmap-force-roaring bitmap:native:bitop:dest

        assert_equal 2 [r bitop or bitmap:native:bitop:dest bitmap:native:bitop:a bitmap:native:bitop:b]
        assert_equal string [r type bitmap:native:bitop:dest]
        assert_equal [binary format H* ffff] [r get bitmap:native:bitop:dest]

        assert_equal 2 [r bitop not bitmap:native:bitop:not bitmap:native:bitop:a]
        assert_equal string [r type bitmap:native:bitop:not]
        assert_equal [binary format H* 0fff] [r get bitmap:native:bitop:not]
    }
}
