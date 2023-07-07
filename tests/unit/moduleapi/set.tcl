set testmodule [file normalize tests/modules/set.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test {Module set add} {
        r del k
        # Check that failure does not create empty key
        assert_error "ERR wrong number of arguments for 'set.add' command" {r set.add k}
        assert_equal 0 [r exists k]

        assert_equal {0} [r set.ismember k hello]
        assert_equal {1} [r set.add k hello]
        assert_equal {1} [r set.ismember k hello]
        assert_equal {0} [r set.ismember k world]
    }

    test {Module set rem} {
        r del k
        assert_equal 0 [r set.rem k hello]
        r sadd k hello world
        assert_equal 1 [r set.rem k hello]
        assert_equal 1 [r exists k]
        # Check that removing the last element deletes the key
        assert_equal 1 [r set.rem k world]
        assert_equal 0 [r exists k]
    }
}
