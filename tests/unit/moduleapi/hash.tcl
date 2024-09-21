set testmodule [file normalize tests/modules/hash.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test {Module hash set} {
        r set k mystring
        assert_error "WRONGTYPE*" {r hash.set k "" hello world}
        r del k
        # "" = count updates and deletes of existing fields only
        assert_equal 0 [r hash.set k "" squirrel yes]
        # "a" = COUNT_ALL = count inserted, modified and deleted fields
        assert_equal 2 [r hash.set k "a" banana no sushi whynot]
        # "n" = NX = only add fields not already existing in the hash
        # "x" = XX = only replace the value for existing fields
        assert_equal 0 [r hash.set k "n" squirrel hoho what nothing]
        assert_equal 1 [r hash.set k "na" squirrel hoho something nice]
        assert_equal 0 [r hash.set k "xa" new stuff not inserted]
        assert_equal 1 [r hash.set k "x" squirrel ofcourse]
        assert_equal 1 [r hash.set k "" sushi :delete: none :delete:]
        r hgetall k
    } {squirrel ofcourse banana no what nothing something nice}

    test {Module hash - set (override) NX expired field successfully} {
        r debug set-active-expire 0
        r del H1 H2
        r hash.set H1 "n" f1 v1
        r hpexpire H1 1 FIELDS 1 f1
        r hash.set H2 "n" f1 v1 f2 v2
        r hpexpire H2 1 FIELDS 1 f1
        after 5
        assert_equal 0 [r hash.set H1 "n" f1 xx]
        assert_equal "f1 xx" [r hgetall H1]
        assert_equal 0 [r hash.set H2 "n" f1 yy]
        assert_equal "f1 f2 v2 yy" [lsort [r hgetall H2]]
        r debug set-active-expire 1
    } {OK} {needs:debug}

    test {Module hash - set XX of expired field gets failed as expected} {
        r debug set-active-expire 0
        r del H1 H2
        r hash.set H1 "n" f1 v1
        r hpexpire H1 1 FIELDS 1 f1
        r hash.set H2 "n" f1 v1 f2 v2
        r hpexpire H2 1 FIELDS 1 f1
        after 5

        # expected to fail on condition XX. hgetall should return empty list
        r hash.set H1 "x" f1 xx
        assert_equal "" [lsort [r hgetall H1]]
        # But expired field was not lazy deleted
        assert_equal 1 [r hlen H1]

        # expected to fail on condition XX. hgetall should return list without expired f1
        r hash.set H2 "x" f1 yy
        assert_equal "f2 v2" [lsort [r hgetall H2]]
        # But expired field was not lazy deleted
        assert_equal 2 [r hlen H2]

        r debug set-active-expire 1
    } {OK} {needs:debug}

    test {test open key with REDISMODULE_OPEN_KEY_ACCESS_EXPIRED to scan expired fields} {
        r debug set-active-expire 0
        r del H1
        r hash.set H1 "n" f1 v1 f2 v2 f3 v3
        r hpexpire H1 1 FIELDS 2 f1 f2
        after 10
        # Scan expired fields with flag REDISMODULE_OPEN_KEY_ACCESS_EXPIRED
        assert_equal "f1 f2 f3 v1 v2 v3" [lsort [r hash.hscan_expired H1]]
        # Get expired field with flag REDISMODULE_OPEN_KEY_ACCESS_EXPIRED
        assert_equal {v1} [r hash.hget_expired H1 f1]
        # Verify key doesn't exist on normal access without the flag
        assert_equal 0 [r hexists H1 f1]
        assert_equal 0 [r hexists H1 f2]
        # Scan again expired fields with flag REDISMODULE_OPEN_KEY_ACCESS_EXPIRED
        assert_equal "f3 v3" [lsort [r hash.hscan_expired H1]]
        r debug set-active-expire 1
    }

    test {test open key with REDISMODULE_OPEN_KEY_ACCESS_EXPIRED to scan expired key} {
        r debug set-active-expire 0
        r del H1
        r hash.set H1 "n" f1 v1 f2 v2 f3 v3
        r pexpire H1 5
        after 10
        # Scan expired fields with flag REDISMODULE_OPEN_KEY_ACCESS_EXPIRED
        assert_equal "f1 f2 f3 v1 v2 v3" [lsort [r hash.hscan_expired H1]]
        # Get expired field with flag REDISMODULE_OPEN_KEY_ACCESS_EXPIRED
        assert_equal {v1} [r hash.hget_expired H1 f1]
        # Verify key doesn't exist on normal access without the flag
        assert_equal 0 [r exists H1]
        r debug set-active-expire 1
    }

    test "Unload the module - hash" {
        assert_equal {OK} [r module unload hash]
    }
}
