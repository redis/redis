start_server {tags {"increx"}} {
    # ---------------------------------------------------------------------
    # Default behavior (no increment option -> +1, create with 0 if missing)
    # ---------------------------------------------------------------------

    test {INCREX - creates key with value 0 then +1 when missing (no options)} {
        r del mykey
        assert_equal [r increx mykey] {1 1}
    }

    test {INCREX - default increment on existing integer key} {
        r set mykey 10
        assert_equal [r increx mykey] {11 1}
    }

    # ---------------------------------------------------------------------
    # BYINT behavior
    # ---------------------------------------------------------------------

    test {INCREX - BYINT positive and negative increments} {
        r set mykey 100
        assert_equal [r increx mykey BYINT 5] {105 5}
        assert_equal [r increx mykey BYINT -10] {95 -10}
        assert_equal [r increx mykey BYINT 0] {95 0}
    }

    test {INCREX - BYINT saturates to UBOUND} {
        r set mykey 50
        assert_equal [r increx mykey BYINT 100 UBOUND 80 SATURATE] {80 30}
        assert_equal [r get mykey] 80
    }

    test {INCREX - BYINT saturates to LBOUND} {
        r set mykey 10
        assert_equal [r increx mykey BYINT -100 LBOUND 0 SATURATE] {0 -10}
        assert_equal [r get mykey] 0
    }

    test {INCREX - BYINT LBOUND and UBOUND together, value already inside range unaffected} {
        r set mykey 5
        assert_equal [r increx mykey BYINT 1 LBOUND 0 UBOUND 10] {6 1}
    }

    test {INCREX - BYINT positive overflow with SATURATE saturates to LLONG_MAX} {
        # LLONG_MAX = 9223372036854775807
        r set mykey 9223372036854775800
        assert_equal [r increx mykey BYINT 9223372036854775800 SATURATE] {9223372036854775807 7}
        assert_equal [r get mykey] 9223372036854775807
    }

    test {INCREX - BYINT positive overflow with SATURATE and UBOUND saturates to UBOUND} {
        # LLONG_MAX = 9223372036854775807
        r set mykey 9223372036854775800
        assert_equal [r increx mykey BYINT 9223372036854775800 UBOUND 9223372036854775807 SATURATE] {9223372036854775807 7}
        assert_equal [r get mykey] 9223372036854775807
    }

    test {INCREX - BYINT negative overflow with SATURATE saturates to LLONG_MIN} {
        # LLONG_MIN = -9223372036854775808
        r set mykey -9223372036854775800
        assert_equal [r increx mykey BYINT -9223372036854775800 SATURATE] {-9223372036854775808 -8}
        assert_equal [r get mykey] -9223372036854775808
    }

    test {INCREX - BYINT negative overflow with SATURATE and LBOUND saturates to LBOUND} {
        # LLONG_MIN = -9223372036854775808
        r set mykey -9223372036854775800
        assert_equal [r increx mykey BYINT -9223372036854775800 LBOUND -9223372036854775808 SATURATE] {-9223372036854775808 -8}
        assert_equal [r get mykey] -9223372036854775808
    }

    test {INCREX - BYINT SATURATE rejects when applied delta would overflow long long} {
        # The saturated result lands at LLONG_MIN while the prior value is positive,
        # so the reported delta would not fit in a long long.
        r set mykey 9223372036854775800
        assert_error "*applied increment would overflow*" {
            r increx mykey BYINT 1 SATURATE UBOUND -9223372036854775808
        }
    }

    test {INCREX - result within [LONG_MIN, LONG_MAX] keeps int encoding} {
        r del mykey
        r increx mykey
        assert_encoding int mykey
        r set mykey 2000000000
        r increx mykey BYINT 100
        assert_encoding int mykey
        r set mykey -2000000000
        r increx mykey BYINT -100
        assert_encoding int mykey
    }

    # ---------------------------------------------------------------------
    # BYFLOAT behavior
    # ---------------------------------------------------------------------

    test {INCREX - BYFLOAT basic increment} {
        r set mykey 1.5
        assert_equal [lmap v [r increx mykey BYFLOAT 0.25] {roundFloat $v}] {1.75 0.25}
        assert_equal [lmap v [r increx mykey BYFLOAT 1] {roundFloat $v}] {2.75 1}
    }

    test {INCREX - BYFLOAT saturates to UBOUND/LBOUND} {
        r set mykey 10
        assert_equal [lmap v [r increx mykey BYFLOAT 100 UBOUND 42.5 SATURATE] {roundFloat $v}] {42.5 32.5}
        r set mykey 0
        assert_equal [lmap v [r increx mykey BYFLOAT -100 LBOUND -5.5 SATURATE] {roundFloat $v}] {-5.5 -5.5}
    }

    # On some platforms strtold("+inf") with valgrind returns a non-inf result
    if {!$::valgrind} {
        test {INCREX - BYFLOAT rejects inf/-inf increment and existing inf/-inf value} {
            # Increment is +inf/-inf -> rejected at parse time.
            r set mykey 0
            assert_error "*BYFLOAT increment cannot be Infinity*" {r increx mykey BYFLOAT +inf}
            assert_error "*BYFLOAT increment cannot be Infinity*" {r increx mykey BYFLOAT -inf}

            # Existing stored value is inf/-inf -> rejected at execution time.
            r set mykey inf
            assert_error "*value cannot be Infinity*" {r increx mykey BYFLOAT 1}
            assert_equal [r get mykey] inf
            r set mykey -inf
            assert_error "*value cannot be Infinity*" {r increx mykey BYFLOAT 0 LBOUND -100}
            assert_equal [r get mykey] -inf
        }
    }

    # ---------------------------------------------------------------------
    # Non-existent key whose default 0 is already outside [LBOUND, UBOUND]
    # and the increment cannot bring it back into range -> default policy
    # leaves the key absent and replies [0, 0].
    # ---------------------------------------------------------------------

    test {INCREX - BYINT/BYFLOAT on non-existent key refuses to create when result stays below LBOUND} {
        r del mykey
        assert_equal [r increx mykey BYINT 5 LBOUND 10] {0 0}
        assert_equal [r exists mykey] 0

        assert_equal [lmap v [r increx mykey BYFLOAT -0.5 UBOUND -1.5] {roundFloat $v}] {0 0}
        assert_equal [r exists mykey] 0
    }

    # ---------------------------------------------------------------------
    # Existing key whose value is already outside [LBOUND, UBOUND] is treated
    # the same as an in-range value pushed outside by the increment: the
    # default policy leaves the key alone and SATURATE saturates.
    # ---------------------------------------------------------------------

    test {INCREX - BYFLOAT existing value already outside bounds} {
        # Above UBOUND, same-side increment: default leaves value unchanged, SATURATE saturates to UBOUND.
        r set mykey 50.5
        assert_equal [lmap v [r increx mykey BYFLOAT 5.5 UBOUND 30] {roundFloat $v}] {50.5 0}
        assert_equal [roundFloat [r get mykey]] 50.5
        assert_equal [lmap v [r increx mykey BYFLOAT 5.5 UBOUND 30 SATURATE] {roundFloat $v}] {30 -20.5}

        # Below LBOUND, same-side decrement: SATURATE saturates to LBOUND.
        r set mykey -50.5
        assert_equal [lmap v [r increx mykey BYFLOAT -5.5 LBOUND -30 SATURATE] {roundFloat $v}] {-30 20.5}

        # Increment that brings the out-of-range value back inside is applied normally.
        r set mykey 50
        assert_equal [lmap v [r increx mykey BYFLOAT -25 UBOUND 30] {roundFloat $v}] {25 -25}
    }

    test {INCREX - BYINT existing value already outside bounds} {
        # Above UBOUND, same-side increment: default leaves value unchanged, SATURATE saturates to UBOUND.
        r set mykey 50
        assert_equal [r increx mykey BYINT 5 UBOUND 30] {50 0}
        assert_equal [r get mykey] 50
        assert_equal [r increx mykey BYINT 5 UBOUND 30 SATURATE] {30 -20}

        # Below LBOUND, same-side decrement: SATURATE saturates to LBOUND.
        r set mykey -50
        assert_equal [r increx mykey BYINT -5 LBOUND -30 SATURATE] {-30 20}

        # Increment that brings the out-of-range value back inside is applied normally.
        r set mykey 50
        assert_equal [r increx mykey BYINT -25 UBOUND 30] {25 -25}
    }

    # ---------------------------------------------------------------------
    # Out-of-range behavior: by default the operation is rejected
    # (reply is [current_value, 0]); SATURATE saturates the result.
    # ---------------------------------------------------------------------

    test {INCREX - BYINT default rejects increment exceeding UBOUND; SATURATE saturates it} {
        r set mykey 10
        assert_equal [r increx mykey BYINT 10 UBOUND 15] {10 0}
        # Value is unchanged
        assert_equal [r get mykey] 10
        # SATURATE saturates the result at UBOUND
        assert_equal [r increx mykey BYINT 10 UBOUND 15 SATURATE] {15 5}
        assert_equal [r get mykey] 15
    }

    test {INCREX - BYINT default rejects decrement falling below LBOUND; SATURATE floors it} {
        r set mykey 10
        assert_equal [r increx mykey BYINT -10 LBOUND 5] {10 0}
        assert_equal [r get mykey] 10
        # SATURATE floors the result at LBOUND
        assert_equal [r increx mykey BYINT -10 LBOUND 5 SATURATE] {5 -5}
        assert_equal [r get mykey] 5
    }

    test {INCREX - BYINT within bounds is unaffected by SATURATE} {
        r set mykey 10
        assert_equal [r increx mykey BYINT 3 UBOUND 20] {13 3}
        assert_equal [r increx mykey BYINT -3 LBOUND 0 SATURATE] {10 -3}
        assert_equal [r increx mykey BYINT 1 UBOUND 20] {11 1}
    }

    test {INCREX - BYINT with both LBOUND and UBOUND} {
        r set mykey 5
        # Within range -> allowed
        assert_equal [r increx mykey BYINT 2 LBOUND 0 UBOUND 10] {7 2}
        # Exceeds UBOUND -> rejected, value unchanged
        assert_equal [r increx mykey BYINT 10 LBOUND 0 UBOUND 10] {7 0}
        # Falls below LBOUND -> rejected, value unchanged
        assert_equal [r increx mykey BYINT -20 LBOUND 0 UBOUND 10] {7 0}
        assert_equal [r get mykey] 7
        # SATURATE saturates at the bounds
        assert_equal [r increx mykey BYINT 10 LBOUND 0 UBOUND 10 SATURATE] {10 3}
        assert_equal [r increx mykey BYINT -20 LBOUND 0 UBOUND 10 SATURATE] {0 -10}
    }

    test {INCREX - BYINT at exact bound value is accepted} {
        r set mykey 5
        # Increment that lands exactly on UBOUND -> allowed
        assert_equal [r increx mykey BYINT 5 UBOUND 10] {10 5}
        # Decrement that lands exactly on LBOUND -> allowed
        assert_equal [r increx mykey BYINT -10 LBOUND 0] {0 -10}
    }

    test {INCREX - BYFLOAT default rejects increment exceeding UBOUND; SATURATE saturates it} {
        r set mykey 10.0
        assert_equal [lmap v [r increx mykey BYFLOAT 10.0 UBOUND 15.5] {roundFloat $v}] {10 0}
        assert_equal [roundFloat [r get mykey]] 10
        # SATURATE saturates the result at UBOUND
        assert_equal [lmap v [r increx mykey BYFLOAT 10.0 UBOUND 15.5 SATURATE] {roundFloat $v}] {15.5 5.5}
    }

    test {INCREX - BYFLOAT default rejects decrement falling below LBOUND; SATURATE floors it} {
        r set mykey 10.0
        assert_equal [lmap v [r increx mykey BYFLOAT -10.0 LBOUND 5.5] {roundFloat $v}] {10 0}
        assert_equal [roundFloat [r get mykey]] 10
        # SATURATE floors the result at LBOUND
        assert_equal [lmap v [r increx mykey BYFLOAT -10.0 LBOUND 5.5 SATURATE] {roundFloat $v}] {5.5 -4.5}
    }

    test {INCREX - BYFLOAT within bounds is unaffected by SATURATE policy} {
        r set mykey 1.5
        assert_equal [lmap v [r increx mykey BYFLOAT 0.25 UBOUND 10.0] {roundFloat $v}] {1.75 0.25}
        assert_equal [lmap v [r increx mykey BYFLOAT 0.25 UBOUND 10.0 SATURATE] {roundFloat $v}] {2 0.25}
    }

    test {INCREX - BYFLOAT with both LBOUND and UBOUND} {
        r set mykey 5.0
        # Within range -> allowed
        assert_equal [lmap v [r increx mykey BYFLOAT 1.5 LBOUND 0 UBOUND 10] {roundFloat $v}] {6.5 1.5}
        # Exceeds UBOUND -> rejected
        assert_equal [lmap v [r increx mykey BYFLOAT 10 LBOUND 0 UBOUND 10] {roundFloat $v}] {6.5 0}
        # Falls below LBOUND -> rejected
        assert_equal [lmap v [r increx mykey BYFLOAT -20 LBOUND 0 UBOUND 10] {roundFloat $v}] {6.5 0}
        assert_equal [lmap v [r get mykey] {roundFloat $v}] {6.5}
    }

    test {INCREX - BYFLOAT at exact bound value is accepted} {
        r set mykey 5.0
        assert_equal [lmap v [r increx mykey BYFLOAT 5.0 UBOUND 10.0] {roundFloat $v}] {10 5}
        assert_equal [lmap v [r increx mykey BYFLOAT -10.0 LBOUND 0] {roundFloat $v}] {0 -10}
    }

    test {INCREX - BYINT positive overflow: default rejects, SATURATE saturates} {
        # LLONG_MAX = 9223372036854775807
        r set mykey 9223372036854775800
        assert_equal [r increx mykey BYINT 9223372036854775800 UBOUND 9223372036854775807] {9223372036854775800 0}
        assert_equal [r get mykey] 9223372036854775800
        # SATURATE: overflow saturates to LLONG_MAX, then saturates to UBOUND
        assert_equal [r increx mykey BYINT 9223372036854775800 UBOUND 9223372036854775807 SATURATE] {9223372036854775807 7}
    }

    test {INCREX - BYINT negative overflow: default rejects, SATURATE saturates} {
        # LLONG_MIN = -9223372036854775808
        r set mykey -9223372036854775800
        assert_equal [r increx mykey BYINT -9223372036854775800 LBOUND -9223372036854775808] {-9223372036854775800 0}
        assert_equal [r get mykey] -9223372036854775800
        # SATURATE: overflow saturates to LLONG_MIN, then saturates to LBOUND
        assert_equal [r increx mykey BYINT -9223372036854775800 LBOUND -9223372036854775808 SATURATE] {-9223372036854775808 -8}
    }

    test {INCREX - BYINT on new key (created from zero) with bound} {
        r del mykey
        # Increment from 0 stays within UBOUND -> allowed
        assert_equal [r increx mykey BYINT 5 UBOUND 10] {5 5}
        r del mykey
        # Increment from 0 exceeds UBOUND -> rejected, key not created
        assert_equal [r increx mykey BYINT 15 UBOUND 10] {0 0}
        assert_equal [r exists mykey] 0
    }

    test {INCREX - BYFLOAT on new key (created from zero) with bound} {
        r del mykey
        # Increment from 0 stays within UBOUND -> allowed
        assert_equal [lmap v [r increx mykey BYFLOAT 5.5 UBOUND 10] {roundFloat $v}] {5.5 5.5}
        r del mykey
        # Increment from 0 exceeds UBOUND -> rejected, key not created
        assert_equal [lmap v [r increx mykey BYFLOAT 15.5 UBOUND 10] {roundFloat $v}] {0 0}
        assert_equal [r exists mykey] 0
    }

    test {INCREX - default with no bound saturates to type limits with SATURATE, rejects otherwise} {
        # In-range increments behave like INCRBY/INCRBYFLOAT.
        r set mykey 10
        assert_equal [r increx mykey BYINT 1] {11 1}
        assert_equal [lmap v [r increx mykey BYFLOAT 1.0] {roundFloat $v}] {12 1}
        assert_equal [r increx mykey] {13 1}

        # BYINT overflow without an explicit bound -> default rejects (reply [current, 0]).
        r set mykey 9223372036854775800
        assert_equal [r increx mykey BYINT 9223372036854775800] {9223372036854775800 0}
        assert_equal [r get mykey] 9223372036854775800
    }

    test {INCREX - reject aborts before side effects: neither value nor TTL is modified} {
        r del mykey
        r set mykey 10
        # An out-of-range result aborts the command before any side effect.
        assert_equal [r increx mykey BYINT 100 UBOUND 15 EX 100] {10 0}
        assert_equal [r get mykey] 10
        assert_equal [r ttl mykey] -1

        r del mykey
        r set mykey 10
        # In-range increment with EX still updates the TTL.
        assert_equal [r increx mykey BYINT 3 UBOUND 20 EX 200] {13 3}
        assert_morethan [r ttl mykey] 0

        r del mykey
        r set mykey 10
        # SATURATE also updates the TTL when saturation kicks in.
        assert_equal [r increx mykey BYINT 100 UBOUND 15 SATURATE EX 200] {15 5}
        assert_morethan [r ttl mykey] 0
    }

    # ---------------------------------------------------------------------
    # Argument parsing / syntax validation
    # ---------------------------------------------------------------------

    test {INCREX - wrong number of arguments} {
        assert_error "*wrong number of arguments*" {r increx}
    }

    test {INCREX - unknown argument} {
        assert_error "*syntax error*" {r increx mykey FOO}
        assert_error "*syntax error*" {r increx mykey BYINT 1 FOO}
    }

    test {INCREX - BYINT and BYFLOAT are mutually exclusive} {
        assert_error "*syntax error*" {r increx mykey BYINT 1 BYFLOAT 1.5}
        assert_error "*syntax error*" {r increx mykey BYFLOAT 1.5 BYINT 1}
    }

    test {INCREX - multiple expiration flags are mutually exclusive} {
        assert_error "*syntax error*" {r increx mykey BYINT 1 EX 10 PX 5000}
        assert_error "*syntax error*" {r increx mykey BYINT 1 EX 10 EXAT 9999999999}
        assert_error "*syntax error*" {r increx mykey BYINT 1 PX 5000 PXAT 9999999999000}
        assert_error "*syntax error*" {r increx mykey BYINT 1 EX 10 PERSIST}
        assert_error "*syntax error*" {r increx mykey BYINT 1 PERSIST EX 10}
    }

    test {INCREX - PERSIST and ENX are mutually exclusive} {
        assert_error "*syntax error*" {r increx mykey BYINT 1 PERSIST ENX}
        assert_error "*syntax error*" {r increx mykey BYINT 1 ENX PERSIST}
    }

    test {INCREX - duplicate options are rejected} {
        assert_error "*syntax error*" {r increx mykey BYINT 1 BYINT 2}
        assert_error "*syntax error*" {r increx mykey BYFLOAT 1.0 BYFLOAT 2.0}
        assert_error "*syntax error*" {r increx mykey LBOUND 0 LBOUND 1}
        assert_error "*syntax error*" {r increx mykey UBOUND 9 UBOUND 8}
        assert_error "*syntax error*" {r increx mykey SATURATE SATURATE LBOUND 0}
        assert_error "*syntax error*" {r increx mykey SAT LBOUND 0}
        assert_error "*syntax error*" {r increx mykey ENX ENX EX 10}
        assert_error "*syntax error*" {r increx mykey PERSIST PERSIST}
        assert_error "*syntax error*" {r increx mykey EX 10 EX 20}
        assert_error "*syntax error*" {r increx mykey PX 10 PX 20}
        assert_error "*syntax error*" {r increx mykey EXAT 9999999999 EXAT 9999999998}
        assert_error "*syntax error*" {r increx mykey PXAT 9999999999000 PXAT 9999999998000}
    }

    test {INCREX - ENX without expiration is an error} {
        assert_error "*ENX flag requires an expiration*" {r increx mykey BYINT 1 ENX}
        assert_error "*ENX flag requires an expiration*" {r increx mykey ENX}
    }

    test {INCREX - BYINT requires a valid integer value} {
        assert_error "*Increment is not an integer*" {r increx mykey BYINT abc}
        assert_error "*Increment is not an integer*" {r increx mykey BYINT 1.5}
    }

    test {INCREX - BYFLOAT requires a valid float value} {
        assert_error "*Increment is not a valid float*" {r increx mykey BYFLOAT abc}
    }

    test {INCREX - LBOUND > UBOUND should be rejected (integer)} {
        assert_error "*LBOUND can't be greater than UBOUND*" {r increx mykey BYINT 1 LBOUND 10 UBOUND 5}
    }

    test {INCREX - LBOUND > UBOUND should be rejected (float)} {
        assert_error "*LBOUND can't be greater than UBOUND*" {r increx mykey BYFLOAT 0.5 LBOUND 10.5 UBOUND 1.5}
    }

    test {INCREX - EX/PX non-positive value is rejected} {
        assert_error "*invalid expire time*" {r increx mykey BYINT 1 EX 0}
        assert_error "*invalid expire time*" {r increx mykey BYINT 1 PX 0}
        assert_error "*invalid expire time*" {r increx mykey BYINT 1 EX -1}
    }

    # ---------------------------------------------------------------------
    # Type check
    # ---------------------------------------------------------------------

    test {INCREX - WRONGTYPE against a list} {
        r del mylist
        r rpush mylist a b c
        assert_error "WRONGTYPE*" {r increx mylist}
        assert_error "WRONGTYPE*" {r increx mylist BYINT 1}
        assert_error "WRONGTYPE*" {r increx mylist BYFLOAT 1.5}
    }

    test {INCREX - WRONGTYPE when BYFLOAT applied to non-numeric string} {
        r set mykey "hello"
        assert_error "*value is not a valid float*" {r increx mykey BYFLOAT 1.5}
        assert_error "*value is not an integer*" {r increx mykey BYINT 1}
    }

    # ---------------------------------------------------------------------
    # Expiration handling
    # ---------------------------------------------------------------------

    test {INCREX - EX sets TTL (seconds)} {
        r del mykey
        r increx mykey BYINT 1 EX 100
        assert_morethan [r ttl mykey] 0
        assert_lessthan_equal [r ttl mykey] 100
    }

    test {INCREX - PX sets TTL (milliseconds)} {
        r del mykey
        r increx mykey BYINT 1 PX 100000
        assert_morethan [r pttl mykey] 0
        assert_lessthan_equal [r pttl mykey] 100000
    }

    test {INCREX - EXAT sets absolute TTL (seconds)} {
        r del mykey
        set ts [expr [clock seconds] + 100]
        r increx mykey BYINT 1 EXAT $ts
        assert_morethan [r ttl mykey] 0
        assert_lessthan_equal [r ttl mykey] 100
    }

    test {INCREX - PXAT sets absolute TTL (milliseconds)} {
        r del mykey
        set ts [expr [clock milliseconds] + 100000]
        r increx mykey BYINT 1 PXAT $ts
        assert_morethan [r pttl mykey] 0
        assert_lessthan_equal [r pttl mykey] 100000
    }

    test {INCREX - without expiration option preserves existing TTL} {
        r del mykey
        r set mykey 5 EX 1000
        set old_ttl [r ttl mykey]
        r increx mykey BYINT 1
        set new_ttl [r ttl mykey]
        # Existing TTL is preserved (should be within a small delta of old_ttl)
        assert_morethan $new_ttl [expr $old_ttl - 5]
    }

    test {INCREX - PERSIST removes existing TTL} {
        r set mykey 5 EX 1000
        assert_morethan [r ttl mykey] 0
        r increx mykey BYINT 1 PERSIST
        assert_equal [r ttl mykey] -1
    }

    test {INCREX - PERSIST on key without TTL leaves it TTL-less} {
        r del mykey
        r set mykey 10
        r increx mykey BYINT 1 PERSIST
        assert_equal [r ttl mykey] -1
    }

    test {INCREX - ENX only sets TTL when key has no existing TTL} {
        # Case 1: key exists with no TTL -> ENX sets the TTL
        r del mykey
        r set mykey 10
        assert_equal [r ttl mykey] -1
        r increx mykey BYINT 1 EX 100 ENX
        assert_morethan [r ttl mykey] 0
        assert_lessthan_equal [r ttl mykey] 100

        # Case 2: key already has TTL -> ENX must NOT touch it
        r del mykey
        r set mykey 10 EX 500
        set old_ttl [r ttl mykey]
        r increx mykey BYINT 1 EX 10 ENX
        set new_ttl [r ttl mykey]
        # TTL should not have been shortened to ~10s
        assert_morethan $new_ttl 490
        # Value should have been incremented
        assert_equal [r get mykey] 11
    }

    test {INCREX - ENX on new key sets TTL (no existing expiry)} {
        r del mykey
        r increx mykey BYINT 5 EX 100 ENX
        assert_morethan [r ttl mykey] 0
        assert_equal [r get mykey] 5
    }

    test {INCREX - EXAT in the past deletes the key} {
        r del mykey
        r set mykey 10
        # An expiration time clearly in the past
        r increx mykey BYINT 1 EXAT 1
        assert_equal [r exists mykey] 0
    }

    test {INCREX - PXAT in the past deletes the key} {
        r del mykey
        r set mykey 10
        r increx mykey BYINT 1 PXAT 1
        assert_equal [r exists mykey] 0
    }

    test {INCREX - ENX skips deletion when key already has TTL and past EXAT is given} {
        r del mykey
        r set mykey 10 EX 500
        # ENX means "only set TTL if key has no TTL" - the past EXAT must not
        # cause deletion because ENX prevents the TTL from being modified.
        r increx mykey BYINT 1 EXAT 1 ENX
        assert_equal [r exists mykey] 1
        assert_equal [r get mykey] 11
        # Old TTL is preserved
        assert_morethan [r ttl mykey] 100
    }

    # ---------------------------------------------------------------------
    # Order-independent / flexible argument ordering
    # ---------------------------------------------------------------------

    test {INCREX - flags can appear in different orders} {
        r del mykey
        # Expiration before increment spec
        r increx mykey EX 100 BYINT 5
        assert_equal [r get mykey] 5
        assert_morethan [r ttl mykey] 0

        # LBOUND/UBOUND interleaved with increment
        r set mykey 5
        assert_equal [r increx mykey LBOUND 0 BYINT 100 UBOUND 10 SATURATE] {10 5}
    }

    # ---------------------------------------------------------------------
    # Command rewrite / replication propagation
    #
    # INCREX is always propagated as a SET command carrying the final value.
    # The exact rewrite depends on TTL-related options:
    #
    #   (a) no expiration option         -> SET <key> <result> KEEPTTL
    #   (b) PERSIST (with existing TTL)  -> SET <key> <result>
    #   (c) EX/PX/EXAT/PXAT              -> SET <key> <result> PXAT <ms>
    #   (d) ENX and key already has TTL  -> SET <key> <result> KEEPTTL
    #   (e) ENX and key has no TTL       -> SET <key> <result> PXAT <ms>
    #   (f) expiration already elapsed   -> DEL/UNLINK <key>
    # ---------------------------------------------------------------------

    test {INCREX - rewrite without expiration: SET key <result> KEEPTTL} {
        r flushall
        set repl [attach_to_replication_stream]
        r set mykey 10
        r increx mykey BYINT 5
        r increx mykey BYFLOAT 0.5
        assert_replication_stream $repl {
            {select *}
            {set mykey 10*}
            {set mykey 15 KEEPTTL}
            {set mykey 15.5 KEEPTTL}
        }
        close_replication_stream $repl
    }

    test {INCREX - rewrite with PERSIST on a key with TTL: SET key <result>} {
        r flushall
        set repl [attach_to_replication_stream]
        r set mykey 10 EX 500
        r increx mykey BYINT 1 PERSIST
        assert_replication_stream $repl {
            {select *}
            {set mykey 10 PXAT *}
            {set mykey 11}
        }
        close_replication_stream $repl
    }

    test {INCREX - rewrite with EX/PX/EXAT/PXAT: SET key <result> PXAT *} {
        r flushall
        set repl [attach_to_replication_stream]
        r set mykey 10
        r increx mykey BYINT 1 EX 100
        r increx mykey BYINT 1 PX 100000
        r increx mykey BYINT 1 EXAT [expr [clock seconds] + 100]
        r increx mykey BYINT 1 PXAT [expr [clock milliseconds] + 100000]
        assert_replication_stream $repl {
            {select *}
            {set mykey 10*}
            {set mykey 11 PXAT *}
            {set mykey 12 PXAT *}
            {set mykey 13 PXAT *}
            {set mykey 14 PXAT *}
        }
        close_replication_stream $repl
    }

    test {INCREX - rewrite with ENX on key that already has TTL: SET key <result> KEEPTTL} {
        r flushall
        set repl [attach_to_replication_stream]
        r set mykey 10 EX 500
        # ENX must preserve the existing TTL, so the rewrite must use KEEPTTL
        # rather than an absolute PXAT derived from the new EX argument.
        r increx mykey BYINT 1 EX 10 ENX
        assert_replication_stream $repl {
            {select *}
            {set mykey 10 PXAT *}
            {set mykey 11 KEEPTTL}
        }
        close_replication_stream $repl
    }

    test {INCREX - rewrite with ENX on key without TTL: SET key <result> PXAT *} {
        r flushall
        set repl [attach_to_replication_stream]
        r set mykey 10
        # No existing TTL, so ENX does set one and we propagate PXAT.
        r increx mykey BYINT 1 EX 100 ENX
        assert_replication_stream $repl {
            {select *}
            {set mykey 10*}
            {set mykey 11 PXAT *}
        }
        close_replication_stream $repl
    }

    test {INCREX - rewrite when expiration already elapsed propagates as DEL} {
        r flushall
        r config set lazyfree-lazy-expire no
        set repl [attach_to_replication_stream]
        r set mykey 10
        r increx mykey BYINT 1 EXAT 1
        assert_equal [r exists mykey] 0
        assert_replication_stream $repl {
            {select *}
            {set mykey 10*}
            {del mykey}
        }
        close_replication_stream $repl
    }

    test {INCREX - rewrite when expiration already elapsed propagates as UNLINK (lazyfree)} {
        r flushall
        r config set lazyfree-lazy-expire yes
        set repl [attach_to_replication_stream]
        r set mykey 10
        r increx mykey BYINT 1 PXAT 1
        assert_equal [r exists mykey] 0
        assert_replication_stream $repl {
            {select *}
            {set mykey 10*}
            {unlink mykey}
        }
        close_replication_stream $repl
        r config set lazyfree-lazy-expire no
    }

    test {INCREX - rewrite carries saturated value after UBOUND/LBOUND} {
        r flushall
        set repl [attach_to_replication_stream]
        r set mykey 50
        # With UBOUND + SATURATE the final value is saturated; the SET
        # rewrite must carry the saturated value (80), not the unbounded 150.
        r increx mykey BYINT 100 UBOUND 80 SATURATE
        r set myfloat 10
        r increx myfloat BYFLOAT 100 UBOUND 42.5 SATURATE
        assert_replication_stream $repl {
            {select *}
            {set mykey 50*}
            {set mykey 80 KEEPTTL}
            {set myfloat 10*}
            {set myfloat 42.5 KEEPTTL}
        }
        close_replication_stream $repl
    }

    test {INCREX - rewrite creates the key from zero when key did not exist} {
        r flushall
        set repl [attach_to_replication_stream]
        r increx mykey BYINT 7
        assert_replication_stream $repl {
            {select *}
            {set mykey 7 KEEPTTL}
        }
        close_replication_stream $repl
    }

    test {INCREX - keyspace notifications fire expected events in order} {
        r flushall
        r config set notify-keyspace-events KEA
        set rd [redis_deferring_client]
        assert_equal {1} [psubscribe $rd __keyevent@*__:*]

        # BYINT -> "incrby"
        r increx k BYINT 5
        assert_match "*__keyevent*incrby*k*" [$rd read]

        # BYFLOAT -> "incrbyfloat"
        r increx k BYFLOAT 0.5
        assert_match "*__keyevent*incrbyfloat*k*" [$rd read]

        # PERSIST on key with TTL -> "incrby" then "persist"
        r set k 10 EX 100
        assert_match "*set*"    [$rd read]
        assert_match "*expire*" [$rd read]
        r increx k BYINT 1 PERSIST
        assert_match "*__keyevent*incrby*k*"  [$rd read]
        assert_match "*__keyevent*persist*k*" [$rd read]

        # EX -> "incrby" then "expire"
        r increx k BYINT 1 EX 100
        assert_match "*__keyevent*incrby*k*" [$rd read]
        assert_match "*__keyevent*expire*k*" [$rd read]

        # ENX on key with TTL: only "incrby", no "expire" (probe with DEL).
        r increx k BYINT 1 EX 200 ENX
        assert_match "*__keyevent*incrby*k*" [$rd read]
        r del k
        assert_match "*__keyevent*del*k*" [$rd read]

        # Past EXAT: early-return branch, only "del".
        r set k 10
        assert_match "*set*" [$rd read]
        r increx k BYINT 1 EXAT 1
        assert_match "*__keyevent*del*k*" [$rd read]

        $rd close
    }
}
