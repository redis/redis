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

    test {PFGETREG returns the HyperLogLog raw registers} {
        r del hll
        r pfadd hll 1 2 3
        llength [r pfgetreg hll]
    } {16384}
}
