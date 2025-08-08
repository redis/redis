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
}
