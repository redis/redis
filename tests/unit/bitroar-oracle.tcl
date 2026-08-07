# Helpers for comparing legacy string bitmap behavior with Roaring bitmap
# behavior. Each registered mode replays the same scenario steps in its own
# keyspace and the replies must match exactly. The roaring mode runs the server
# in bitmap-default-roaring yes so every bitmap write creates or converts to
# Roaring bitmaps; scenarios observe only bitmap-level behavior (never TYPE or
# OBJECT ENCODING), which is exactly the parity the exposure gate demands.

namespace eval bitmap_oracle {
    variable modes {legacy-string roaring}
}

proc bitmap_oracle::modes {} {
    variable modes
    return $modes
}

proc bitmap_oracle::set_modes {new_modes} {
    variable modes
    set modes $new_modes
}

proc bitmap_oracle::mode_setup {client mode} {
    switch -- $mode {
        legacy-string {
            $client config set bitmap-default-roaring no
            return
        }
        roaring {
            $client config set bitmap-default-roaring yes
            return
        }
        default {
            error "unknown bitmap oracle mode '$mode'"
        }
    }
}

proc bitmap_oracle::keyspace_name {scenario mode} {
    set safe_scenario [string map {" " "_" ":" "_" "/" "_"} $scenario]
    set safe_mode [string map {" " "_" ":" "_" "/" "_"} $mode]
    return "bitmap-oracle:{$safe_scenario:$safe_mode}"
}

proc bitmap_oracle::expand_step {step keyspace} {
    set expanded {}
    foreach arg $step {
        lappend expanded [string map [list %NS% $keyspace] $arg]
    }
    return $expanded
}

proc bitmap_oracle::call {client command} {
    set invocation [linsert $command 0 $client]
    set status [catch {uplevel #0 $invocation} reply]
    if {$status == 0} {
        return [list ok $reply]
    }
    return [list err $reply]
}

proc bitmap_oracle::run_steps {client mode scenario steps} {
    bitmap_oracle::mode_setup $client $mode

    set keyspace [bitmap_oracle::keyspace_name $scenario $mode]
    set results {}
    foreach step $steps {
        set command [bitmap_oracle::expand_step $step $keyspace]
        lappend results [bitmap_oracle::call $client $command]
    }
    return $results
}

proc bitmap_oracle::assert_mode_equivalence {client scenario steps} {
    set baseline {}
    set baseline_mode {}
    foreach mode [bitmap_oracle::modes] {
        set results [bitmap_oracle::run_steps $client $mode $scenario $steps]
        if {$baseline_mode eq {}} {
            set baseline $results
            set baseline_mode $mode
        } elseif {$results ne $baseline} {
            error "bitmap oracle mismatch between '$baseline_mode' and '$mode': expected '$baseline', got '$results'"
        }
    }
    return $baseline
}

start_server {tags {"bitmap" "bitmap-oracle"}} {
    test {bitmap roaring oracle exposes legacy and roaring modes} {
        assert_equal [bitmap_oracle::modes] {legacy-string roaring}
    }

    test {bitmap roaring oracle records sparse SETBIT behavior} {
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

    test {bitmap roaring oracle covers BITFIELD and range read parity} {
        set steps {
            {del %NS%:bitmap}
            {setbit %NS%:bitmap 0 1}
            {setbit %NS%:bitmap 9 1}
            {setbit %NS%:bitmap 31 1}
            {bitcount %NS%:bitmap}
            {bitcount %NS%:bitmap 4 24 bit}
            {bitpos %NS%:bitmap 1 8 -1 bit}
            {bitfield %NS%:bitmap GET u10 0 SET u5 20 17 GET u8 16}
            {bitfield_ro %NS%:bitmap GET u5 20 GET u1 31}
            {getbit %NS%:bitmap 24}
            {bitcount %NS%:bitmap}
        }

        set got [bitmap_oracle::assert_mode_equivalence r bitfield-ranges $steps]
        set expected [list \
            [list ok 0] \
            [list ok 0] \
            [list ok 0] \
            [list ok 0] \
            [list ok 3] \
            [list ok 1] \
            [list ok 9] \
            [list ok {513 0 8}] \
            [list ok {17 1}] \
            [list ok 1] \
            [list ok 5]]
        assert_equal $got $expected
    }

    test {bitmap roaring oracle covers BITOP aliasing and missing sources} {
        set steps {
            {del %NS%:a %NS%:b %NS%:c %NS%:out %NS%:not}
            {setbit %NS%:a 0 1}
            {setbit %NS%:a 2 1}
            {setbit %NS%:a 9 1}
            {setbit %NS%:a 16 1}
            {setbit %NS%:b 1 1}
            {setbit %NS%:b 2 1}
            {setbit %NS%:b 16 1}
            {setbit %NS%:b 17 1}
            {bitop xor %NS%:a %NS%:a %NS%:b}
            {bitcount %NS%:a}
            {bitop or %NS%:out %NS%:a %NS%:missing %NS%:c}
            {bitcount %NS%:out}
            {bitop not %NS%:not %NS%:out}
            {bitcount %NS%:not}
            {bitpos %NS%:not 0}
        }

        set got [bitmap_oracle::assert_mode_equivalence r bitop-alias-missing $steps]
        set expected [list \
            [list ok 0] \
            [list ok 0] \
            [list ok 0] \
            [list ok 0] \
            [list ok 0] \
            [list ok 0] \
            [list ok 0] \
            [list ok 0] \
            [list ok 0] \
            [list ok 3] \
            [list ok 4] \
            [list ok 3] \
            [list ok 4] \
            [list ok 3] \
            [list ok 20] \
            [list ok 0]]
        assert_equal $got $expected
    }

    test {bitmap roaring oracle ports deterministic redis-roaring fuzz seed cases} {
        set scenarios {
            {seed1_setbit {
                {del %NS%:bitmap}
                {setbit %NS%:bitmap 1 1}
                {setbit %NS%:bitmap 64 1}
                {setbit %NS%:bitmap 4096 1}
                {getbit %NS%:bitmap 64}
                {bitcount %NS%:bitmap}
                {bitpos %NS%:bitmap 1}
                {bitfield_ro %NS%:bitmap GET u1 1 GET u1 2 GET u1 4096}
            } {
                {ok 0}
                {ok 0}
                {ok 0}
                {ok 0}
                {ok 1}
                {ok 3}
                {ok 1}
                {ok {1 0 1}}
            }}
            {seed4_diff_alias {
                {del %NS%:a %NS%:b %NS%:out}
                {setbit %NS%:a 0 1}
                {setbit %NS%:a 2 1}
                {setbit %NS%:b 2 1}
                {setbit %NS%:b 3 1}
                {bitop diff %NS%:a %NS%:a %NS%:b}
                {bitcount %NS%:a}
                {bitop one %NS%:out %NS%:a %NS%:b}
                {bitcount %NS%:out}
            } {
                {ok 0}
                {ok 0}
                {ok 0}
                {ok 0}
                {ok 0}
                {ok 1}
                {ok 1}
                {ok 1}
                {ok 3}
            }}
            {seed5_setfull_bounded {
                {del %NS%:bitmap}
                {bitfield %NS%:bitmap SET u8 0 255}
                {bitcount %NS%:bitmap}
                {bitpos %NS%:bitmap 0}
                {bitfield %NS%:bitmap SET u5 8 31}
                {bitcount %NS%:bitmap}
                {bitpos %NS%:bitmap 0}
            } {
                {ok 0}
                {ok 0}
                {ok 8}
                {ok 8}
                {ok 0}
                {ok 13}
                {ok 13}
            }}
        }

        foreach scenario $scenarios {
            lassign $scenario name steps expected
            set expected [lrange $expected 0 end]
            set got [bitmap_oracle::assert_mode_equivalence r "redis-roaring:$name" $steps]
            assert_equal $got $expected
        }
    }

    test {legacy bitmap strings keep string command behavior} {
        bitmap_oracle::mode_setup r legacy-string
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
        bitmap_oracle::mode_setup r legacy-string
        set corpus {
            {single-zero {0}}
            {byte-boundaries {0 7 8 15}}
            {word-boundaries {15 16 31 32 63 64 127 128}}
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
        bitmap_oracle::mode_setup r legacy-string
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
