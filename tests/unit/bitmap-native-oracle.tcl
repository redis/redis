source tests/support/bitmap_oracle.tcl

start_server {tags {"bitmap" "bitmap-oracle"}} {
    test {bitmap native oracle exposes legacy-string baseline mode} {
        assert_equal [bitmap_oracle::modes] {legacy-string}
    }

    test {bitmap native oracle records sparse SETBIT behavior} {
        set steps {
            {del %NS%:bitmap}
            {setbit %NS%:bitmap 0 1}
            {setbit %NS%:bitmap 2 1}
            {setbit %NS%:bitmap 65536 1}
            {getbit %NS%:bitmap 65536}
            {bitcount %NS%:bitmap}
            {bitpos %NS%:bitmap 1}
            {bitpos %NS%:bitmap 0}
            {bitfield %NS%:bitmap GET u1 0 GET u1 2 GET u1 65536}
        }

        set got [bitmap_oracle::assert_mode_equivalence r sparse-setbit $steps]
        set expected [list \
            [list ok 0] \
            [list ok 0] \
            [list ok 0] \
            [list ok 0] \
            [list ok 1] \
            [list ok 3] \
            [list ok 0] \
            [list ok 1] \
            [list ok {1 1 1}]]
        assert_equal $got $expected
    }

    test {legacy bitmap strings keep string command behavior} {
        set key bitmap:legacy-boundary{t}
        r del $key

        assert_equal 0 [r setbit $key 9 1]
        assert_equal string [r type $key]
        assert_equal 2 [r strlen $key]
        assert_equal [binary format B* 0000000001000000] [r get $key]
        assert_equal 3 [r append $key Z]
        assert_equal 3 [r strlen $key]
        assert_equal 3 [r setrange $key 0 abc]
        assert_equal abc [r get $key]
    }

    test {legacy bitmap corpus covers sparse and boundary offsets} {
        set corpus {
            {single-zero {0}}
            {byte-boundaries {0 7 8 15}}
            {container-boundaries {4095 4096 65535 65536 131071}}
            {large-sparse {1 1000 100000 1000000}}
        }

        foreach fixture $corpus {
            lassign $fixture name offsets
            set key "bitmap:corpus:$name{t}"
            r del $key

            foreach offset $offsets {
                assert_equal 0 [r setbit $key $offset 1]
                assert_equal 1 [r getbit $key $offset]
            }

            set sorted [lsort -integer $offsets]
            set max [lindex $sorted end]
            assert_equal [llength $offsets] [r bitcount $key]
            assert_equal [lindex $sorted 0] [r bitpos $key 1]
            assert_equal [expr {$max / 8 + 1}] [r strlen $key]
            assert_equal string [r type $key]
        }
    }

    test {legacy bitmap deterministic sparse fuzz corpus} {
        for {set j 0} {$j < 16} {incr j} {
            set key "bitmap:fuzz:$j{t}"
            r del $key
            catch {unset seen}
            array set seen {}

            for {set i 0} {$i < 64} {incr i} {
                set offset [expr {(($j + 1) * 97 + ($i * $i * 131)) % 250000}]
                set seen($offset) 1
                r setbit $key $offset 1
                assert_equal 1 [r getbit $key $offset]
            }

            assert_equal [array size seen] [r bitcount $key]
            assert_equal string [r type $key]
        }
    }
}
