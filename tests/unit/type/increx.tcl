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

    test {INCREX - BYINT clamps to UBOUND} {
        r set mykey 50
        assert_equal [r increx mykey BYINT 100 UBOUND 80] {80 30}
        assert_equal [r get mykey] 80
    }

    test {INCREX - BYINT clamps to LBOUND} {
        r set mykey 10
        assert_equal [r increx mykey BYINT -100 LBOUND 0] {0 -10}
        assert_equal [r get mykey] 0
    }

    test {INCREX - BYINT LBOUND and UBOUND together, value already inside range unaffected} {
        r set mykey 5
        assert_equal [r increx mykey BYINT 1 LBOUND 0 UBOUND 10] {6 1}
    }

    test {INCREX - BYINT positive overflow returns an error} {
        # LLONG_MAX = 9223372036854775807
        r set mykey 9223372036854775800
        assert_error {ERR increment or decrement would overflow*} {r increx mykey BYINT 9223372036854775800}
    }

    test {INCREX - BYINT positive overflow saturates to UBOUND when explicitly specified} {
        # LLONG_MAX = 9223372036854775807
        r set mykey 9223372036854775800
        assert_equal [r increx mykey BYINT 9223372036854775800 UBOUND 9223372036854775807] {9223372036854775807 7}
        assert_equal [r get mykey] 9223372036854775807
    }

    test {INCREX - BYINT negative overflow returns an error} {
        # LLONG_MIN = -9223372036854775808
        r set mykey -9223372036854775800
        assert_error {ERR increment or decrement would overflow} {r increx mykey BYINT -9223372036854775800}
    }

    test {INCREX - BYINT negative overflow saturates to LBOUND when explicitly specified} {
        # LLONG_MIN = -9223372036854775808
        r set mykey -9223372036854775800
        assert_equal [r increx mykey BYINT -9223372036854775800 LBOUND -9223372036854775808] {-9223372036854775808 -8}
        assert_equal [r get mykey] -9223372036854775808
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

    test {INCREX - BYFLOAT clamps to UBOUND} {
        r set mykey 10
        assert_equal [lmap v [r increx mykey BYFLOAT 100 UBOUND 42.5] {roundFloat $v}] {42.5 32.5}
    }

    test {INCREX - BYFLOAT clamps to LBOUND} {
        r set mykey 0
        assert_equal [lmap v [r increx mykey BYFLOAT -100 LBOUND -5.5] {roundFloat $v}] {-5.5 -5.5}
    }

    test {INCREX - BYFLOAT positive infinity returns an error} {
        r set mykey 0
        assert_error "*would produce Infinity*" {r increx mykey BYFLOAT +inf}
    }

    test {INCREX - BYFLOAT positive infinity saturates to UBOUND when explicitly specified} {
        r set mykey 0
        assert_equal [lmap v [r increx mykey BYFLOAT +inf UBOUND 1000] {roundFloat $v}] {1000 1000}
    }

    test {INCREX - BYFLOAT negative infinity returns an error} {
        r set mykey 0
        assert_error "*would produce Infinity*" {r increx mykey BYFLOAT -inf}
    }

    test {INCREX - BYFLOAT negative infinity saturates to LBOUND when explicitly specified} {
        r set mykey 0
        assert_equal [lmap v [r increx mykey BYFLOAT -inf LBOUND -1000] {roundFloat $v}] {-1000 -1000}
    }

    # ---------------------------------------------------------------------
    # Strict behavior
    # ---------------------------------------------------------------------

    test {INCREX - STRICT BYINT rejects increment exceeding UBOUND} {
        r set mykey 10
        # Without STRICT: value would be clamped to 15
        assert_equal [r increx mykey BYINT 10 UBOUND 15] {15 5}
        # With STRICT: value exceeds UBOUND, so the update is rejected
        r set mykey 10
        assert_equal [r increx mykey BYINT 10 UBOUND 15 STRICT] {10 0}
        # Verify the key value is unchanged
        assert_equal [r get mykey] 10
    }

    test {INCREX - STRICT BYINT rejects decrement exceeding LBOUND} {
        r set mykey 10
        # Without STRICT: value would be clamped to 5
        assert_equal [r increx mykey BYINT -10 LBOUND 5] {5 -5}
        # With STRICT: value exceeds LBOUND, so the update is rejected
        r set mykey 10
        assert_equal [r increx mykey BYINT -10 LBOUND 5 STRICT] {10 0}
        assert_equal [r get mykey] 10
    }

    test {INCREX - STRICT BYINT allows increment within bounds} {
        r set mykey 10
        # Increment stays within bounds -> STRICT does not interfere
        assert_equal [r increx mykey BYINT 3 UBOUND 20 STRICT] {13 3}
        assert_equal [r get mykey] 13
    }

    test {INCREX - STRICT BYINT allows decrement within bounds} {
        r set mykey 10
        assert_equal [r increx mykey BYINT -3 LBOUND 0 STRICT] {7 -3}
        assert_equal [r get mykey] 7
    }

    test {INCREX - STRICT BYINT with both LBOUND and UBOUND} {
        r set mykey 5
        # Within range -> allowed
        assert_equal [r increx mykey BYINT 2 LBOUND 0 UBOUND 10 STRICT] {7 2}
        # Exceeds UBOUND -> rejected
        assert_equal [r increx mykey BYINT 10 LBOUND 0 UBOUND 10 STRICT] {7 0}
        # Exceeds LBOUND -> rejected
        assert_equal [r increx mykey BYINT -20 LBOUND 0 UBOUND 10 STRICT] {7 0}
        assert_equal [r get mykey] 7
    }

    test {INCREX - STRICT BYINT at exact bound value is accepted} {
        r set mykey 5
        # Increment that lands exactly on UBOUND -> allowed
        assert_equal [r increx mykey BYINT 5 UBOUND 10 STRICT] {10 5}
        # Decrement that lands exactly on LBOUND -> allowed
        assert_equal [r increx mykey BYINT -10 LBOUND 0 STRICT] {0 -10}
    }

    test {INCREX - STRICT BYFLOAT rejects increment exceeding UBOUND} {
        r set mykey 10.0
        # Without STRICT: clamped to 15.5
        assert_equal [lmap v [r increx mykey BYFLOAT 10.0 UBOUND 15.5] {roundFloat $v}] {15.5 5.5}
        # With STRICT: rejected, reverts to old value
        r set mykey 10.0
        assert_equal [lmap v [r increx mykey BYFLOAT 10.0 UBOUND 15.5 STRICT] {roundFloat $v}] {10 0}
    }

    test {INCREX - STRICT BYFLOAT rejects decrement exceeding LBOUND} {
        r set mykey 10.0
        # Without STRICT: clamped to 5.5
        assert_equal [lmap v [r increx mykey BYFLOAT -10.0 LBOUND 5.5] {roundFloat $v}] {5.5 -4.5}
        # With STRICT: rejected
        r set mykey 10.0
        assert_equal [lmap v [r increx mykey BYFLOAT -10.0 LBOUND 5.5 STRICT] {roundFloat $v}] {10 0}
    }

    test {INCREX - STRICT BYFLOAT allows increment within bounds} {
        r set mykey 1.5
        assert_equal [lmap v [r increx mykey BYFLOAT 0.25 UBOUND 10.0 STRICT] {roundFloat $v}] {1.75 0.25}
    }

    test {INCREX - STRICT BYFLOAT with both LBOUND and UBOUND} {
        r set mykey 5.0
        # Within range -> allowed
        assert_equal [lmap v [r increx mykey BYFLOAT 1.5 LBOUND 0 UBOUND 10 STRICT] {roundFloat $v}] {6.5 1.5}
        # Exceeds UBOUND -> rejected
        assert_equal [lmap v [r increx mykey BYFLOAT 10 LBOUND 0 UBOUND 10 STRICT] {roundFloat $v}] {6.5 0}
        # Exceeds LBOUND -> rejected
        assert_equal [lmap v [r increx mykey BYFLOAT -20 LBOUND 0 UBOUND 10 STRICT] {roundFloat $v}] {6.5 0}
    }

    test {INCREX - STRICT BYFLOAT at exact bound value is accepted} {
        r set mykey 5.0
        assert_equal [lmap v [r increx mykey BYFLOAT 5.0 UBOUND 10.0 STRICT] {roundFloat $v}] {10 5}
        assert_equal [lmap v [r increx mykey BYFLOAT -10.0 LBOUND 0 STRICT] {roundFloat $v}] {0 -10}
    }

    test {INCREX - STRICT BYINT positive overflow with UBOUND rejects instead of saturating} {
        # LLONG_MAX = 9223372036854775807
        r set mykey 9223372036854775800
        # Without STRICT: overflow saturates then clamps to UBOUND
        assert_equal [r increx mykey BYINT 9223372036854775800 UBOUND 9223372036854775807] {9223372036854775807 7}
        # With STRICT: overflow exceeds bound, so reject
        r set mykey 9223372036854775800
        assert_equal [r increx mykey BYINT 9223372036854775800 UBOUND 9223372036854775807 STRICT] {9223372036854775800 0}
        assert_equal [r get mykey] 9223372036854775800
    }

    test {INCREX - STRICT BYINT negative overflow with LBOUND rejects instead of saturating} {
        # LLONG_MIN = -9223372036854775808
        r set mykey -9223372036854775800
        # Without STRICT: overflow saturates then clamps to LBOUND
        assert_equal [r increx mykey BYINT -9223372036854775800 LBOUND -9223372036854775808] {-9223372036854775808 -8}
        # With STRICT: overflow exceeds bound, so reject
        r set mykey -9223372036854775800
        assert_equal [r increx mykey BYINT -9223372036854775800 LBOUND -9223372036854775808 STRICT] {-9223372036854775800 0}
        assert_equal [r get mykey] -9223372036854775800
    }

    test {INCREX - STRICT BYFLOAT positive infinity with UBOUND rejects instead of saturating} {
        r set mykey 0
        # Without STRICT: +inf saturates then clamps to UBOUND
        assert_equal [lmap v [r increx mykey BYFLOAT +inf UBOUND 1000] {roundFloat $v}] {1000 1000}
        # With STRICT: +inf would exceed UBOUND, so reject
        r set mykey 0
        assert_equal [lmap v [r increx mykey BYFLOAT +inf UBOUND 1000 STRICT] {roundFloat $v}] {0 0}
        assert_equal [r get mykey] 0
    }

    test {INCREX - STRICT BYFLOAT negative infinity with LBOUND rejects instead of saturating} {
        r set mykey 0
        # Without STRICT: -inf saturates then clamps to LBOUND
        assert_equal [lmap v [r increx mykey BYFLOAT -inf LBOUND -1000] {roundFloat $v}] {-1000 -1000}
        # With STRICT: -inf would exceed LBOUND, so reject
        r set mykey 0
        assert_equal [lmap v [r increx mykey BYFLOAT -inf LBOUND -1000 STRICT] {roundFloat $v}] {0 0}
        assert_equal [r get mykey] 0
    }

    test {INCREX - STRICT on new key (created from zero)} {
        r del mykey
        # Increment from 0 stays within UBOUND -> allowed
        assert_equal [r increx mykey BYINT 5 UBOUND 10 STRICT] {5 5}
        r del mykey
        # Increment from 0 exceeds UBOUND -> rejected, key still created at 0
        assert_equal [r increx mykey BYINT 15 UBOUND 10 STRICT] {0 0}
        assert_equal [r get mykey] 0
    }

    test {INCREX - STRICT on new key with BYFLOAT (created from zero)} {
        r del mykey
        # Increment from 0 stays within UBOUND -> allowed
        assert_equal [lmap v [r increx mykey BYFLOAT 5.5 UBOUND 10 STRICT] {roundFloat $v}] {5.5 5.5}
        r del mykey
        # Increment from 0 exceeds UBOUND -> rejected, key created at 0
        assert_equal [lmap v [r increx mykey BYFLOAT 15.5 UBOUND 10 STRICT] {roundFloat $v}] {0 0}
    }

    test {INCREX - STRICT without any bound is a syntax error} {
        r set mykey 10
        assert_error "*syntax error*" {r increx mykey BYINT 1 STRICT}
        assert_error "*syntax error*" {r increx mykey BYFLOAT 1.0 STRICT}
        assert_error "*syntax error*" {r increx mykey STRICT}
    }

    test {INCREX - STRICT combined with expiration options} {
        r del mykey
        r set mykey 10
        # STRICT rejection should still preserve TTL behavior
        r increx mykey BYINT 100 UBOUND 15 STRICT EX 100
        # Value should be unchanged (rejected by STRICT)
        assert_equal [r get mykey] 10
        # TTL should still be set even though value was not changed
        assert_morethan [r ttl mykey] 0

        r del mykey
        r set mykey 10
        # STRICT allows within bounds with EX
        assert_equal [r increx mykey BYINT 3 UBOUND 20 STRICT EX 200] {13 3}
        assert_morethan [r ttl mykey] 0
    }

    # ---------------------------------------------------------------------
    # Argument parsing / syntax validation
    # ---------------------------------------------------------------------

    test {INCREX - wrong number of arguments} {
        assert_error "*wrong number of arguments*" {r increx}
    }

    test {INCREX - unknown argument} {
        r del mykey
        assert_error "*syntax error*" {r increx mykey FOO}
        assert_error "*syntax error*" {r increx mykey BYINT 1 FOO}
    }

    test {INCREX - BYINT and BYFLOAT are mutually exclusive} {
        r del mykey
        assert_error "*syntax error*" {r increx mykey BYINT 1 BYFLOAT 1.5}
        assert_error "*syntax error*" {r increx mykey BYFLOAT 1.5 BYINT 1}
    }

    test {INCREX - multiple expiration flags are mutually exclusive} {
        r del mykey
        assert_error "*syntax error*" {r increx mykey BYINT 1 EX 10 PX 5000}
        assert_error "*syntax error*" {r increx mykey BYINT 1 EX 10 EXAT 9999999999}
        assert_error "*syntax error*" {r increx mykey BYINT 1 PX 5000 PXAT 9999999999000}
        assert_error "*syntax error*" {r increx mykey BYINT 1 EX 10 PERSIST}
        assert_error "*syntax error*" {r increx mykey BYINT 1 PERSIST EX 10}
    }

    test {INCREX - PERSIST and ENX are mutually exclusive} {
        r del mykey
        assert_error "*syntax error*" {r increx mykey BYINT 1 PERSIST ENX}
        assert_error "*syntax error*" {r increx mykey BYINT 1 ENX PERSIST}
    }

    test {INCREX - ENX without expiration is a syntax error} {
        r del mykey
        assert_error "*syntax error*" {r increx mykey BYINT 1 ENX}
        assert_error "*syntax error*" {r increx mykey ENX}
    }

    test {INCREX - BYINT requires a valid integer value} {
        r del mykey
        assert_error "*value is not an integer*" {r increx mykey BYINT abc}
        assert_error "*value is not an integer*" {r increx mykey BYINT 1.5}
    }

    test {INCREX - BYFLOAT requires a valid float value} {
        r del mykey
        assert_error "*value is not a valid float*" {r increx mykey BYFLOAT abc}
    }

    test {INCREX - LBOUND > UBOUND should be rejected (integer)} {
        r del mykey
        assert_error "*LBOUND can't be greater than UBOUND*" {r increx mykey BYINT 1 LBOUND 10 UBOUND 5}
    }

    test {INCREX - LBOUND > UBOUND should be rejected (float)} {
        r del mykey
        assert_error "*LBOUND can't be greater than UBOUND*" {r increx mykey BYFLOAT 0.5 LBOUND 10.5 UBOUND 1.5}
    }

    test {INCREX - EX/PX non-positive value is rejected} {
        r del mykey
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
        r del mylist
    }

    test {INCREX - WRONGTYPE when BYFLOAT applied to non-numeric string} {
        r set mykey "hello"
        assert_error "*value is not a valid float*" {r increx mykey BYFLOAT 1.5}
        assert_error "*value is not an integer*" {r increx mykey BYINT 1}
        r del mykey
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
    # Return value format
    # ---------------------------------------------------------------------

    test {INCREX - reply is always an array of [new-value, actual-increment]} {
        r del mykey
        assert_equal [r increx mykey BYINT 42] {42 42}
        assert_equal [r increx mykey BYINT 8 UBOUND 45] {45 3}
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
        assert_equal [r increx mykey LBOUND 0 BYINT 100 UBOUND 10] {10 5}
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

    test {INCREX - rewrite carries clamped value after UBOUND/LBOUND} {
        r flushall
        set repl [attach_to_replication_stream]
        r set mykey 50
        # With UBOUND the final value is clamped; the SET rewrite must
        # carry the clamped value (80), not the unbounded 150.
        r increx mykey BYINT 100 UBOUND 80
        r set myfloat 10
        r increx myfloat BYFLOAT 100 UBOUND 42.5
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
}
