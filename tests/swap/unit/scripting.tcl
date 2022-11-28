start_server {tags {"swap-scripting"}} {
    test "eval - del cold key" {
        r hset h1 k1 v1
        wait_key_cold r h1
        assert_equal [r eval {return redis.call('DEL', KEYS[1])} 1 h1] 1
        assert_equal 0 [r exists h1]
    }

    test "eval - reload after eval" {
        r hset h1 k1 v1
        r hset h2 k1 v1
        wait_key_cold r h1
        wait_key_cold r h2
        r eval {return redis.call('DEL', KEYS[1])} 1 h1
        r eval {return redis.call('HSET', KEYS[1], ARGV[1], 2)} 1 h2 k1
        r eval {return redis.call('HSET', KEYS[1], ARGV[1], 1)} 1 h3 k1
        r hset h1 k2 2
        r debug reload
        assert_equal {k2 2} [r hgetall h1]
        assert_equal {k1 2} [r hgetall h2]
        assert_equal {k1 1} [r hgetall h3]
    }
}
