start_server {tags {"hll"}} {
    test {HyperLogLog self test passes} {
        catch {r pfselftest} e
        set e
    } {OK}

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
    }

    test {HyperLogLog sparse encoding stress test} {
        for {set x 0} {$x < 1000} {incr x} {
            r del hll1 hll2
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
    }

    test {Corrupted sparse HyperLogLogs are detected: Additionl at tail} {
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

    test {Corrupted dense HyperLogLogs are detected: Wrong length} {
        r del hll
        r pfadd hll a b c
        r setrange hll 4 "\x00"
        set e {}
        catch {r pfcount hll} e
        set e
    } {*WRONGTYPE*}

    test {PFADD, PFCOUNT, PFMERGE type checking works} {
        r set foo bar
        catch {r pfadd foo 1} e
        assert_match {*WRONGTYPE*} $e
        catch {r pfcount foo} e
        assert_match {*WRONGTYPE*} $e
        catch {r pfmerge bar foo} e
        assert_match {*WRONGTYPE*} $e
        catch {r pfmerge foo bar} e
        assert_match {*WRONGTYPE*} $e
    }

    test {PFMERGE results on the cardinality of union of sets} {
        r del hll hll1 hll2 hll3
        r pfadd hll1 a b c
        r pfadd hll2 b c d
        r pfadd hll3 c d e
        r pfmerge hll hll1 hll2 hll3
        r pfcount hll
    } {5}

    test {PFCOUNT multiple-keys merge returns cardinality of union #1} {
        r del hll1 hll2 hll3
        for {set x 1} {$x < 10000} {incr x} {
            r pfadd hll1 "foo-$x"
            r pfadd hll2 "bar-$x"
            r pfadd hll3 "zap-$x"

            set card [r pfcount hll1 hll2 hll3]
            set realcard [expr {$x*3}]
            set err [expr {abs($card-$realcard)}]
            assert {$err < (double($card)/100)*5}
        }
    }

    test {PFCOUNT multiple-keys merge returns cardinality of union #2} {
        r del hll1 hll2 hll3
        set elements {}
        for {set x 1} {$x < 10000} {incr x} {
            for {set j 1} {$j <= 3} {incr j} {
                set rint [randomInt 20000]
                r pfadd hll$j $rint
                lappend elements $rint
            }
        }
        set realcard [llength [lsort -unique $elements]]
        set card [r pfcount hll1 hll2 hll3]
        set err [expr {abs($card-$realcard)}]
        assert {$err < (double($card)/100)*5}
    }

    test {PFDEBUG GETREG returns the HyperLogLog raw registers} {
        r del hll
        r pfadd hll 1 2 3
        llength [r pfdebug getreg hll]
    } {16384}

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
