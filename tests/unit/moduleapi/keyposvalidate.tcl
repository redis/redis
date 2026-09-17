# Test: validate that module API key/channel position filtering works.
#
# These tests verify the fix in moduleGetCommandKeysViaAPI and
# moduleGetCommandChannelsViaAPI: positions declared by a module that
# are <= 0 or >= argc are silently filtered out before being used by
# ACL checks (which would otherwise read argv[pos]->ptr OOB and crash
# Redis with SIGSEGV).
#
# Before the fix: the tests below would crash Redis.
# After the fix: they return OK and the invalid positions are ignored.

set testmodule [file normalize tests/modules/keyposvalidate.so]

start_server {tags {"modules external:skip"}} {
    r module load $testmodule

    # ----------------------------------------------------------------
    # Key-position tests
    # ----------------------------------------------------------------

    test "Module getkeys-api: pos=0 is filtered out (no crash)" {
        # Should not crash; COMMAND GETKEYS should return empty list
        # since the only declared position (0) is invalid.
        assert_equal {} [r command getkeys keypos.validate.zero foo]
    }

    test "Module getkeys-api: pos=argc is filtered out (no crash)" {
        # argc=2 (command + 1 arg). pos=2 is invalid (max valid is argc-1=1).
        assert_equal {} [r command getkeys keypos.validate.at_argc foo]
    }

    test "Module getkeys-api: pos=999 is filtered out (no crash)" {
        assert_equal {} [r command getkeys keypos.validate.above_argc foo]
    }

    test "Module getkeys-api: mixed valid and invalid positions" {
        # keypos.validate.mixed declares: 1, 999, argc, 0, 2 (if argc >= 3)
        # With argc=3 (cmd + 2 args), valid positions are 1 and 2.
        # Expected: keys at positions 1 and 2 (foo, bar).
        assert_equal {foo bar} [r command getkeys keypos.validate.mixed foo bar]
    }

    test "Module getkeys-api: invalid positions don't crash ACL check" {
        # Create a user with restricted key access. The ACL check will
        # iterate over the positions returned by the module; if invalid
        # positions aren't filtered, argv[pos]->ptr causes OOB read.
        # We use +@all and resetkeys, then allow only ~allowedkey:*.
        r ACL setuser testuser on +@all resetkeys ~allowedkey:* ">secret123"

        # All these commands declare invalid key positions. With the fix,
        # the positions are filtered out and ACL check passes (no keys
        # to check). Without the fix, Redis crashes with SIGSEGV.
        assert_equal "OK" [r ACL DRYRUN testuser keypos.validate.zero foo]
        assert_equal "OK" [r ACL DRYRUN testuser keypos.validate.at_argc foo]
        assert_equal "OK" [r ACL DRYRUN testuser keypos.validate.above_argc foo]

        # Cleanup
        r ACL deluser testuser
    }

    test "Module getkeys-api: invalid positions in mixed case don't crash ACL" {
        r ACL setuser testuser2 on +@all resetkeys ~allowedkey:* ">secret123"
        # keypos.validate.mixed declares valid (1, 2) and invalid (999, argc, 0) positions.
        # With the fix, only positions 1 and 2 are checked. foo is not in ~allowedkey:*
        # so ACL denies.
        set result [r ACL DRYRUN testuser2 keypos.validate.mixed foo bar]
        # The user doesn't have access to foo, so ACL should deny.
        assert_match {*no permissions*} $result
        r ACL deluser testuser2
    }

    # ----------------------------------------------------------------
    # Channel-position tests
    # ----------------------------------------------------------------

    test "Module getchannels-api: pos=0 is filtered out (no crash)" {
        assert_equal "OK" [r ACL DRYRUN default chanpos.validate.zero foo]
    }

    test "Module getchannels-api: pos=argc is filtered out (no crash)" {
        assert_equal "OK" [r ACL DRYRUN default chanpos.validate.at_argc foo]
    }

    test "Module getchannels-api: pos=999 is filtered out (no crash)" {
        assert_equal "OK" [r ACL DRYRUN default chanpos.validate.above_argc foo]
    }

    test "Module getchannels-api: invalid positions don't crash restricted user ACL" {
        # Create a user with NO channel access. With invalid positions
        # filtered out, the ACL check sees no channels and passes (no
        # channels to deny). Without the fix, Redis crashes.
        r ACL setuser chanuser on +@all resetchannels ">secret123"
        assert_equal "OK" [r ACL DRYRUN chanuser chanpos.validate.zero foo]
        assert_equal "OK" [r ACL DRYRUN chanuser chanpos.validate.at_argc foo]
        assert_equal "OK" [r ACL DRYRUN chanuser chanpos.validate.above_argc foo]
        r ACL deluser chanuser
    }

    # ----------------------------------------------------------------
    # Cleanup
    # ----------------------------------------------------------------

    test "Unload the keyposvalidate module" {
        assert_equal {OK} [r module unload keyposvalidate]
    }
}
