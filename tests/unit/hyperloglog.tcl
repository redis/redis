start_server {tags {"hll"}} {
    test {HyperLogLog self test passes} {
        catch {r pfselftest} e
        set e
    } {OK} {needs:pfdebug}

    test {PFADD without arguments creates an HLL value} {
        r pfadd hll
        r exists hll
    } {1}

    test {Approximated cardinality after creation is zero} {
        r pfcount hll
    } {0}

    test {PFADD returns 1 when at least 1 reg was modified} {
        r pfadd hll a b c
    } {1}

    test {PFADD returns 0 when no reg was modified} {
        r pfadd hll a b c
    } {0}

    test {PFADD works with empty string (regression)} {
        r pfadd hll ""
    }

    # Note that the self test stresses much better the
    # cardinality estimation error. We are testing just the
    # command implementation itself here.
    test {PFCOUNT returns approximated cardinality of set} {
        r del hll
        set res {}
        r pfadd hll 1 2 3 4 5
        lappend res [r pfcount hll]
        # Call it again to test cached value invalidation.
        r pfadd hll 6 7 8 8 9 10
        lappend res [r pfcount hll]
        set res
    } {5 10}

    test {HyperLogLogs are promote from sparse to dense} {
        r del hll
        r config set hll-sparse-max-bytes 3000
        set n 0
        while {$n < 100000} {
            set elements {}
            for {set j 0} {$j < 100} {incr j} {lappend elements [expr rand()]}
            incr n 100
            r pfadd hll {*}$elements
            set card [r pfcount hll]
            set err [expr {abs($card-$n)}]
            assert {$err < (double($card)/100)*5}
            if {$n < 1000} {
                assert {[r pfdebug encoding hll] eq {sparse}}
            } elseif {$n > 10000} {
                assert {[r pfdebug encoding hll] eq {dense}}
            }
        }
    } {} {needs:pfdebug}

    test {Change hll-sparse-max-bytes} {
        r config set hll-sparse-max-bytes 3000
        r del hll
        r pfadd hll a b c d e d g h i j k
        assert {[r pfdebug encoding hll] eq {sparse}}
        r config set hll-sparse-max-bytes 30
        r pfadd hll new_element
        assert {[r pfdebug encoding hll] eq {dense}}
    } {} {needs:pfdebug}

    test {Hyperloglog promote to dense well in different hll-sparse-max-bytes} {
        set max(0) 100
        set max(1) 500
        set max(2) 3000
        for {set i 0} {$i < [array size max]} {incr i} {
            r config set hll-sparse-max-bytes $max($i)
            r del hll
            r pfadd hll
            set len [r strlen hll]
            while {$len <= $max($i)} {
                assert {[r pfdebug encoding hll] eq {sparse}}
                set elements {}
                for {set j 0} {$j < 10} {incr j} { lappend elements [expr rand()]}
                r pfadd hll {*}$elements
                set len [r strlen hll]
            }
            assert {[r pfdebug encoding hll] eq {dense}}
        }
    } {} {needs:pfdebug}

    test {HyperLogLog sparse encoding stress test} {
        for {set x 0} {$x < 1000} {incr x} {
            r del hll1
            r del hll2
            set numele [randomInt 100]
            set elements {}
            for {set j 0} {$j < $numele} {incr j} {
                lappend elements [expr rand()]
            }
            # Force dense representation of hll2
            r pfadd hll2
            r pfdebug todense hll2
            r pfadd hll1 {*}$elements
            r pfadd hll2 {*}$elements
            assert {[r pfdebug encoding hll1] eq {sparse}}
            assert {[r pfdebug encoding hll2] eq {dense}}
            # Cardinality estimated should match exactly.
            assert {[r pfcount hll1] eq [r pfcount hll2]}
        }
    } {} {needs:pfdebug}

    test {Corrupted sparse HyperLogLogs are detected: Additional at tail} {
        r del hll
        r pfadd hll a b c
        r append hll "hello"
        set e {}
        catch {r pfcount hll} e
        set e
    } {*INVALIDOBJ*}

    test {Corrupted sparse HyperLogLogs are detected: Broken magic} {
        r del hll
        r pfadd hll a b c
        r setrange hll 0 "0123"
        set e {}
        catch {r pfcount hll} e
        set e
    } {*WRONGTYPE*}

    test {Corrupted sparse HyperLogLogs are detected: Invalid encoding} {
        r del hll
        r pfadd hll a b c
        r setrange hll 4 "x"
        set e {}
        catch {r pfcount hll} e
        set e
    } {*WRONGTYPE*}

    test {Corrupted sparse HyperLogLogs doesn't cause overflow and out-of-bounds with XZERO opcode} {
        r del hll
        
        # Create a sparse-encoded HyperLogLog header
        set header "HYLL"
        set payload [binary format c12 {1 0 0 0 0 0 0 0 0 0 0 0}]
        set pl [binary format a4a12 $header $payload]

        # Create an XZERO opcode with the maximum run length of 16384(2^14)
        set runlen [expr 16384 - 1]
        set chunk [binary format cc [expr {0b01000000 | ($runlen >> 8)}] [expr {$runlen & 0xff}]]
        # Fill the HLL with more than 131072(2^17) XZERO opcodes to make the total
        # run length exceed 4GB, will cause an integer overflow.
        set repeat [expr 131072 + 1000]
        for {set i 0} {$i < $repeat} {incr i} {
            append pl $chunk
        }

        # Create a VAL opcode with a value that will cause out-of-bounds.
        append pl [binary format c 0b11111111]
        r set hll $pl

        # This should not overflow and out-of-bounds.
        assert_error {*INVALIDOBJ*} {r pfcount hll hll}
        assert_error {*INVALIDOBJ*} {r pfdebug getreg hll}
        r ping
    }

    test {Corrupted sparse HyperLogLogs doesn't cause overflow and out-of-bounds with ZERO opcode} {
        r del hll
        
        # Create a sparse-encoded HyperLogLog header
        set header "HYLL"
        set payload [binary format c12 {1 0 0 0 0 0 0 0 0 0 0 0}]
        set pl [binary format a4a12 $header $payload]

        # # Create an ZERO opcode with the maximum run length of 64(2^6)
        set chunk [binary format c [expr {0b00000000 | 0x3f}]]
        # Fill the HLL with more than 33554432(2^17) ZERO opcodes to make the total
        # run length exceed 4GB, will cause an integer overflow.
        set repeat [expr 33554432 + 1000]
        for {set i 0} {$i < $repeat} {incr i} {
            append pl $chunk
        }

        # Create a VAL opcode with a value that will cause out-of-bounds.
        append pl [binary format c 0b11111111]
        r set hll $pl

        # This should not overflow and out-of-bounds.
        assert_error {*INVALIDOBJ*} {r pfcount hll hll}
        assert_error {*INVALIDOBJ*} {r pfdebug getreg hll}
        r ping
    }

    test {Corrupted dense HyperLogLogs are detected: Wrong length} {
        r del hll
        r pfadd hll a b c
        r setrange hll 4 "\x00"
        set e {}
        catch {r pfcount hll} e
        set e
    } {*WRONGTYPE*}

    test {Fuzzing dense/sparse encoding: Redis should always detect errors} {
        for {set j 0} {$j < 1000} {incr j} {
            r del hll
            set items {}
            set numitems [randomInt 3000]
            for {set i 0} {$i < $numitems} {incr i} {
                lappend items [expr {rand()}]
            }
            r pfadd hll {*}$items

            # Corrupt it in some random way.
            for {set i 0} {$i < 5} {incr i} {
                set len [r strlen hll]
                set pos [randomInt $len]
                set byte [randstring 1 1 binary]
                r setrange hll $pos $byte
                # Don't modify more bytes 50% of times
                if {rand() < 0.5} break
            }

            # Use the hyperloglog to check if it crashes
            # Redis in some way.
            catch {
                r pfcount hll
            }
        }
    }

    test {PFADD, PFCOUNT, PFMERGE type checking works} {
        r set foo{t} bar
        catch {r pfadd foo{t} 1} e
        assert_match {*WRONGTYPE*} $e
        catch {r pfcount foo{t}} e
        assert_match {*WRONGTYPE*} $e
        catch {r pfmerge bar{t} foo{t}} e
        assert_match {*WRONGTYPE*} $e
        catch {r pfmerge foo{t} bar{t}} e
        assert_match {*WRONGTYPE*} $e
    }

    test {PFMERGE results on the cardinality of union of sets} {
        r del hll{t} hll1{t} hll2{t} hll3{t}
        r pfadd hll1{t} a b c
        r pfadd hll2{t} b c d
        r pfadd hll3{t} c d e
        r pfmerge hll{t} hll1{t} hll2{t} hll3{t}
        r pfcount hll{t}
    } {5}

    test {PFMERGE on missing source keys will create an empty destkey} {
        r del sourcekey{t} sourcekey2{t} destkey{t} destkey2{t}

        assert_equal {OK} [r pfmerge destkey{t} sourcekey{t}]
        assert_equal 1 [r exists destkey{t}]
        assert_equal 0 [r pfcount destkey{t}]

        assert_equal {OK} [r pfmerge destkey2{t} sourcekey{t} sourcekey2{t}]
        assert_equal 1 [r exists destkey2{t}]
        assert_equal 0 [r pfcount destkey{t}]
    }

    test {PFMERGE with one empty input key, create an empty destkey} {
        r del destkey
        assert_equal {OK} [r pfmerge destkey]
        assert_equal 1 [r exists destkey]
        assert_equal 0 [r pfcount destkey]
    }

    test {PFMERGE with one non-empty input key, dest key is actually one of the source keys} {
        r del destkey
        assert_equal 1 [r pfadd destkey a b c]
        assert_equal {OK} [r pfmerge destkey]
        assert_equal 1 [r exists destkey]
        assert_equal 3 [r pfcount destkey]
    }

    test {PFMERGE results with simd} {
        r del hllscalar{t} hllsimd{t} hll1{t} hll2{t} hll3{t}
        for {set x 1} {$x < 2000} {incr x} {
            r pfadd hll1{t} [expr rand()]
        }
        for {set x 1} {$x < 4000} {incr x} {
            r pfadd hll2{t} [expr rand()]
        }
        for {set x 1} {$x < 8000} {incr x} {
            r pfadd hll3{t} [expr rand()]
        }
        assert {[r pfcount hll1{t}] > 0}
        assert {[r pfcount hll2{t}] > 0}
        assert {[r pfcount hll3{t}] > 0}

        r pfdebug simd off
        set scalar [r pfcount hll1{t} hll2{t} hll3{t}]
        r pfdebug simd on
        set simd [r pfcount hll1{t} hll2{t} hll3{t}]
        assert {$scalar > 0}
        assert {$simd > 0}
        assert_equal $scalar $simd

        r pfdebug simd off
        r pfmerge hllscalar{t} hll1{t} hll2{t} hll3{t}
        r pfdebug simd on
        r pfmerge hllsimd{t} hll1{t} hll2{t} hll3{t}

        set scalar [r pfcount hllscalar{t}]
        set simd [r pfcount hllsimd{t}]
        assert {$scalar > 0}
        assert {$simd > 0}
        assert_equal $scalar $simd

        set scalar [r get hllscalar{t}]
        set simd [r get hllsimd{t}]
        assert_equal $scalar $simd

    } {} {needs:pfdebug}

    test {PFCOUNT multiple-keys merge returns cardinality of union #1} {
        r del hll1{t} hll2{t} hll3{t}
        for {set x 1} {$x < 10000} {incr x} {
            r pfadd hll1{t} "foo-$x"
            r pfadd hll2{t} "bar-$x"
            r pfadd hll3{t} "zap-$x"

            set card [r pfcount hll1{t} hll2{t} hll3{t}]
            set realcard [expr {$x*3}]
            set err [expr {abs($card-$realcard)}]
            assert {$err < (double($card)/100)*5}
        }
    }

    test {PFCOUNT multiple-keys merge returns cardinality of union #2} {
        r del hll1{t} hll2{t} hll3{t}
        set elements {}
        for {set x 1} {$x < 10000} {incr x} {
            for {set j 1} {$j <= 3} {incr j} {
                set rint [randomInt 20000]
                r pfadd hll$j{t} $rint
                lappend elements $rint
            }
        }
        set realcard [llength [lsort -unique $elements]]
        set card [r pfcount hll1{t} hll2{t} hll3{t}]
        set err [expr {abs($card-$realcard)}]
        assert {$err < (double($card)/100)*5}
    }

    test {PFDEBUG GETREG returns the HyperLogLog raw registers} {
        r del hll
        r pfadd hll 1 2 3
        llength [r pfdebug getreg hll]
    } {16384} {needs:pfdebug}

    test {PFADD / PFCOUNT cache invalidation works} {
        r del hll
        r pfadd hll a b c
        r pfcount hll
        assert {[r getrange hll 15 15] eq "\x00"}
        r pfadd hll a b c
        assert {[r getrange hll 15 15] eq "\x00"}
        r pfadd hll 1 2 3
        assert {[r getrange hll 15 15] eq "\x80"}
    }

    test {PFADD with 2GB entry should not crash server due to overflow in MurmurHash64A} {
        r config set proto-max-bulk-len 3221225472
        r config set client-query-buffer-limit 3221225472
        r write "*3\r\n\$5\r\nPFADD\r\n\$3\r\nhll\r\n"
        write_big_bulk 2147483648;
        r ping
    } {PONG} {large-memory}

    # Build a raw HLL_ULTRA string: 16-byte header (p in notused[0]) + nregbytes zero registers.
    proc build_ull_blob {p nregbytes} {
        set hdr "HYLL"
        append hdr [binary format c 2]      ;# encoding = HLL_ULTRA
        append hdr [binary format c $p]     ;# notused[0] = p
        append hdr [binary format c2 {0 0}] ;# notused[1..2]
        append hdr [binary format c8 {0 0 0 0 0 0 0 0}] ;# card
        append hdr [string repeat "\x00" $nregbytes]
        return $hdr
    }

    test {ULL: a hand-crafted valid HLL_ULTRA blob validates and reports encoding} {
        r del ull
        r set ull [build_ull_blob 14 16384]
        assert_equal {ultra} [r pfdebug encoding ull]
    } {} {needs:pfdebug}

    test {ULL: wrong-size HLL_ULTRA blob is rejected as invalid} {
        r del ullbad
        r set ullbad [build_ull_blob 14 100]
        assert_error "*WRONGTYPE*" {r pfadd ullbad x}
    }

    test {ULL: out-of-range precision is rejected} {
        r del ullp12{t} ullp16{t}
        r set ullp12{t} [build_ull_blob 12 4096]   ;# p=12 below min, size matches 1<<12
        r set ullp16{t} [build_ull_blob 16 65536]  ;# p=16 above max, size matches 1<<16
        assert_error "*WRONGTYPE*" {r pfadd ullp12{t} x}
        assert_error "*WRONGTYPE*" {r pfadd ullp16{t} x}
    }

    test {ULL: PFSELFTEST ULL passes (codec round-trip + add idempotence)} {
        r pfselftest ull
    } {OK}

    test {ULL: PFSELFTEST rejects unknown subcommands} {
        catch {r pfselftest foo} err
        set err
    } {ERR*}

    test {ULL: PFCOUNT on an ultra key is accurate} {
        r del ua
        r set ua [build_ull_blob 14 16384]
        for {set i 0} {$i < 50000} {incr i} { r pfadd ua "z$i" }
        assert {abs([r pfcount ua] - 50000) < 50000*0.02}
    }

    test {ULL: PFADD with hll-dense-encoding=ultra promotes to ultra encoding} {
        r config set hll-dense-encoding ultra
        r config set hll-sparse-max-bytes 0
        r del u14
        assert_equal 1 [r pfadd u14 a b c d e]
        assert_equal {ultra} [r pfdebug encoding u14]
        assert {abs([r pfcount u14] - 5) <= 1}
        r config set hll-sparse-max-bytes 3000
        r config set hll-dense-encoding classic
    }
    test {ULL: default config keeps classic dense (opt-in)} {
        r del cl
        r config set hll-sparse-max-bytes 0
        r pfadd cl a b c
        assert_equal {dense} [r pfdebug encoding cl]
        r config set hll-sparse-max-bytes 3000
    }
    test {ULL: ultra p=14 promotion stays accurate at scale} {
        r config set hll-dense-encoding ultra
        r config set hll-sparse-max-bytes 0
        r del up14
        for {set i 0} {$i < 30000} {incr i} { r pfadd up14 "e-$i" }
        assert_equal {ultra} [r pfdebug encoding up14]
        assert {abs([r pfcount up14] - 30000) < 30000*0.03}
        r config set hll-sparse-max-bytes 3000
        r config set hll-dense-encoding classic
    }

    test {ULL: PFMERGE of two ultra keys is accurate and idempotent} {
        r config set hll-dense-encoding ultra
        r config set hll-sparse-max-bytes 0
        r del a{t} b{t} d{t}
        for {set i 0} {$i < 20000} {incr i} { r pfadd a{t} "x$i" }
        for {set i 10000} {$i < 30000} {incr i} { r pfadd b{t} "x$i" }
        r pfmerge d{t} a{t} b{t}
        assert_equal {ultra} [r pfdebug encoding d{t}]
        assert {abs([r pfcount d{t}] - 30000) < 30000*0.03}
        set once [r pfcount d{t}]; r pfmerge d{t} a{t} b{t}; assert_equal $once [r pfcount d{t}]
        r config set hll-sparse-max-bytes 3000
        r config set hll-dense-encoding classic
    }

    test {ULL: cross-encoding PFMERGE (classic + ultra) is correct, result classic} {
        r del c{t} u{t} d2{t}
        r config set hll-sparse-max-bytes 0
        r config set hll-dense-encoding classic
        for {set i 0} {$i < 15000} {incr i} { r pfadd c{t} "z$i" }
        r config set hll-dense-encoding ultra
        for {set i 7500} {$i < 22500} {incr i} { r pfadd u{t} "z$i" }
        r pfmerge d2{t} c{t} u{t}
        assert_equal {dense} [r pfdebug encoding d2{t}]
        assert {abs([r pfcount d2{t}] - 22500) < 22500*0.04}
        r config set hll-sparse-max-bytes 3000
        r config set hll-dense-encoding classic
    }

    test {ULL: multi-key PFCOUNT of two ultra keys} {
        r config set hll-dense-encoding ultra
        r config set hll-sparse-max-bytes 0
        r del a{t} b{t}
        for {set i 0} {$i < 20000} {incr i} { r pfadd a{t} "x$i" }
        for {set i 10000} {$i < 30000} {incr i} { r pfadd b{t} "x$i" }
        assert {abs([r pfcount a{t} b{t}] - 30000) < 30000*0.04}
        r config set hll-sparse-max-bytes 3000
        r config set hll-dense-encoding classic
    }

    test {ULL: multi-key PFCOUNT across mixed encodings} {
        r del c{t} u{t}
        r config set hll-sparse-max-bytes 0
        r config set hll-dense-encoding classic
        for {set i 0} {$i < 15000} {incr i} { r pfadd c{t} "z$i" }
        r config set hll-dense-encoding ultra
        for {set i 7500} {$i < 22500} {incr i} { r pfadd u{t} "z$i" }
        assert {abs([r pfcount c{t} u{t}] - 22500) < 22500*0.04}
        r config set hll-sparse-max-bytes 3000
        r config set hll-dense-encoding classic
    }

    test {ULL: classic-only PFMERGE still works (regression)} {
        r del e{t} f{t} g{t}
        r config set hll-dense-encoding classic
        r config set hll-sparse-max-bytes 0
        for {set i 0} {$i < 5000} {incr i} { r pfadd e{t} "p$i" }
        for {set i 2500} {$i < 7500} {incr i} { r pfadd f{t} "p$i" }
        r pfmerge g{t} e{t} f{t}
        assert_equal {dense} [r pfdebug encoding g{t}]
        assert {abs([r pfcount g{t}] - 7500) < 7500*0.05}
        r config set hll-sparse-max-bytes 3000
    }

    test {ULL: mixed PFMERGE into a pre-existing ULL dest works} {
        r config set hll-dense-encoding ultra; r config set hll-sparse-max-bytes 0
        r del md{t} cs{t}
        for {set i 0} {$i < 5000} {incr i} { r pfadd md{t} "m$i" }    ;# md becomes ULL
        assert_equal {ultra} [r pfdebug encoding md{t}]
        r config set hll-dense-encoding classic
        for {set i 2500} {$i < 7500} {incr i} { r pfadd cs{t} "m$i" }  ;# cs is classic
        r pfmerge md{t} cs{t}                                       ;# ULL dest + classic src
        assert_equal {dense} [r pfdebug encoding md{t}]
        assert {abs([r pfcount md{t}] - 7500) < 7500*0.05}
        r config set hll-sparse-max-bytes 3000
    }

    test {ULL: crafted invalid register bytes do not trigger codec UB} {
        # encoding=2 (ultra), p=14 in notused[0], then 16384 register bytes.
        # Real ULL registers are 0 or >= 52; bytes in [4,7] caused a negative shift
        # in ullUnpack, and bytes in {1,2,3} could make ullPack(0)/clz(0) in the
        # all-ULL merge. isHLLObjectOrReply only checks length+precision, so a
        # crafted blob (RESTORE/RDB) reaches these. Must not crash (UBSan: no abort).
        set hdr "HYLL"
        append hdr [binary format c 2] [binary format c 14]
        append hdr [binary format c2 {0 0}] [binary format c8 {0 0 0 0 0 0 0 0}]
        set b1 $hdr; append b1 [string repeat [binary format c 5] 16384] ;# bytes=5, in [4,7]
        set b2 $hdr; append b2 [string repeat [binary format c 2] 16384] ;# bytes=2, in {1,2,3}
        r del cb1{t} cb2{t}
        r set cb1{t} $b1
        r set cb2{t} $b2
        assert_equal {ultra} [r pfdebug encoding cb1{t}]
        r pfadd cb1{t} elem      ;# ullDenseAdd -> ullUnpack on a crafted byte
        r pfcount cb1{t} cb2{t}     ;# all-ULL multi-key -> ullUnpack / ullPack(0) path
        r pfmerge cb1{t} cb1{t} cb2{t} ;# all-ULL merge -> same
        assert {[r pfcount cb1{t}] >= 0} ;# returns a sane number, no crash
    }

    test {ULL: PFDEBUG GETREG returns the ultra registers verbatim} {
        r config set hll-dense-encoding ultra
        r config set hll-sparse-max-bytes 0
        r del ureg
        r pfadd ureg a b c d e
        assert_equal {ultra} [r pfdebug encoding ureg]
        set regs [r pfdebug getreg ureg]
        assert_equal 16384 [llength $regs]
        # A valid ULL register byte is 0 (empty) or >= 52 (4*(p-1) at p=14).
        # The old 6-bit-dense misread would surface small values in [1,51].
        set nonzero 0
        foreach v $regs {
            assert {$v == 0 || $v >= 52}
            if {$v != 0} {incr nonzero}
        }
        assert {$nonzero > 0}
        r config set hll-sparse-max-bytes 3000
        r config set hll-dense-encoding classic
    }

    test {ULL p=13: 8 KB dense blob, ~33% smaller than classic p=14} {
        r config set hll-dense-encoding ultra
        r config set hll-ultra-precision 13
        r config set hll-sparse-max-bytes 0
        r del u13
        r pfadd u13 a b c d e
        assert_equal {ultra} [r pfdebug encoding u13]
        # ULL p=13 has 2^13 one-byte registers (dense blob = 16-byte header +
        # 8192 = 8208 bytes, one third smaller than classic p=14's 12304).
        assert_equal 8192 [llength [r pfdebug getreg u13]]
        r config set hll-ultra-precision 14
        r config set hll-sparse-max-bytes 3000
    }
    test {ULL p=13: PFADD/PFCOUNT accurate at scale} {
        r config set hll-dense-encoding ultra
        r config set hll-ultra-precision 13
        r config set hll-sparse-max-bytes 0
        r del u13
        for {set i 0} {$i < 100000} {incr i} { r pfadd u13 "e-$i" }
        set est [r pfcount u13]
        # p=13 relative std error is ~0.84%; allow a comfortable 4% band so the
        # test is not flaky on a single run.
        assert {abs($est - 100000) < 4000}
        r config set hll-ultra-precision 14
        r config set hll-sparse-max-bytes 3000
    }
    test {ULL p=13: precision survives RDB reload} {
        r config set hll-dense-encoding ultra
        r config set hll-ultra-precision 13
        r config set hll-sparse-max-bytes 0
        r del u13
        r pfadd u13 a b c d e
        set before [r pfcount u13]
        r debug reload
        assert_equal 8192 [llength [r pfdebug getreg u13]]
        assert_equal {ultra} [r pfdebug encoding u13]
        assert_equal $before [r pfcount u13]
        r config set hll-ultra-precision 14
        r config set hll-sparse-max-bytes 3000
    }
    test {ULL p=13: same-precision PFMERGE and multi-key PFCOUNT work} {
        r config set hll-dense-encoding ultra
        r config set hll-ultra-precision 13
        r config set hll-sparse-max-bytes 0
        r del a13 b13 dst13
        for {set i 0} {$i < 20000} {incr i} { r pfadd a13 "x-$i" }
        for {set i 10000} {$i < 30000} {incr i} { r pfadd b13 "x-$i" }
        # union has 30000 distinct elements.
        set merged [r pfcount a13 b13]
        assert {abs($merged - 30000) < 1500}
        r pfmerge dst13 a13 b13
        assert_equal {ultra} [r pfdebug encoding dst13]
        assert_equal 8192 [llength [r pfdebug getreg dst13]]
        assert_equal $merged [r pfcount dst13]
        # Idempotent re-merge.
        r pfmerge dst13 a13 b13
        assert_equal $merged [r pfcount dst13]
        r config set hll-ultra-precision 14
        r config set hll-sparse-max-bytes 3000
    }
    test {ULL p=13: cross-precision PFMERGE/PFCOUNT is rejected} {
        r config set hll-dense-encoding ultra
        r config set hll-sparse-max-bytes 0
        r config set hll-ultra-precision 13
        r del a13
        r pfadd a13 x y z
        r config set hll-ultra-precision 14
        r del c14
        r pfadd c14 p q r
        assert_error {*different precisions*} {r pfcount a13 c14}
        assert_error {*different precisions*} {r pfmerge dst a13 c14}
        r config set hll-sparse-max-bytes 3000
    }
    test {ULL type: ultra config promotes new key to OBJ_HLL} {
        r config set hll-dense-encoding ultra
        r del t
        r pfadd t a b c d e
        assert_equal {hll} [r type t]
        assert_equal {raw} [r object encoding t]
        r config set hll-dense-encoding classic
    }
    test {ULL type: classic config keeps OBJ_STRING (regression)} {
        r config set hll-dense-encoding classic
        r del t
        r pfadd t a b c d e
        assert_equal {string} [r type t]
    }
    test {ULL type: existing string HLL is promoted lazily on write} {
        r config set hll-dense-encoding classic
        r del t
        r pfadd t a b c
        assert_equal {string} [r type t]
        r config set hll-dense-encoding ultra
        # A read leaves the type unchanged...
        r pfcount t
        r pfdebug encoding t
        assert_equal {string} [r type t]
        # ...the first write promotes it.
        r pfadd t x y z
        assert_equal {hll} [r type t]
        r config set hll-dense-encoding classic
    }
    test {ULL type: MEMORY USAGE and OBJECT work on an OBJ_HLL key} {
        r config set hll-dense-encoding ultra
        r del t
        r pfadd t a b c d e
        assert_equal {hll} [r type t]
        assert {[r memory usage t] > 0}
        assert_equal {raw} [r object encoding t]
        r config set hll-dense-encoding classic
    }
    test {ULL type: string commands are rejected on an OBJ_HLL key} {
        r config set hll-dense-encoding ultra
        r del t
        r pfadd t a b c
        assert_equal {hll} [r type t]
        assert_error {WRONGTYPE*} {r get t}
        assert_error {WRONGTYPE*} {r append t x}
        assert_error {WRONGTYPE*} {r setrange t 0 x}
        assert_error {WRONGTYPE*} {r strlen t}
        r config set hll-dense-encoding classic
    }
    test {ULL type: RDB reload preserves type, count and digest} {
        r config set hll-dense-encoding ultra
        r del t
        r pfadd t {*}[lrange [split [string repeat "x " 200]] 0 199]
        set before [r pfcount t]
        set dig [r debug digest-value t]
        r debug reload
        assert_equal {hll} [r type t]
        assert_equal $before [r pfcount t]
        assert_equal $dig [r debug digest-value t]
        r config set hll-dense-encoding classic
    }
    test {ULL type: DUMP/RESTORE preserves the OBJ_HLL type} {
        r config set hll-dense-encoding ultra
        r del t t2
        r pfadd t a b c d e
        set d [r dump t]
        r restore t2 0 $d
        assert_equal {hll} [r type t2]
        assert_equal [r pfcount t] [r pfcount t2]
        r config set hll-dense-encoding classic
    }
    test {ULL type: COPY preserves the OBJ_HLL type} {
        r config set hll-dense-encoding ultra
        r del t t2
        r pfadd t a b c d e
        r copy t t2
        assert_equal {hll} [r type t2]
        assert_equal [r pfcount t] [r pfcount t2]
        r config set hll-dense-encoding classic
    }
    test {ULL type: AOF rewrite reconstructs the OBJ_HLL key} {
        r config set hll-dense-encoding ultra
        r config set appendonly yes
        waitForBgrewriteaof r
        r del t
        r pfadd t a b c d e
        set before [r pfcount t]
        r bgrewriteaof
        waitForBgrewriteaof r
        r debug loadaof
        assert_equal {hll} [r type t]
        assert_equal $before [r pfcount t]
        r config set appendonly no
        r config set hll-dense-encoding classic
    }
}
