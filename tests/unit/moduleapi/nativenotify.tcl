set testmodule [file normalize tests/modules/nativenotify.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test {Module API native notifications: SET} {
        r nn.set k a
        r nn.set k b
        r nn.set k c
        assert_equal [r hget notifications set] 3
        r del k notifications
    }

    test {Module API native notifications: TRUNCATE} {
        r nn.set k abcdefg
        r nn.truncate k 10
        r nn.truncate k 5
        # Same length, no notification
        r nn.truncate k 5
        # New key
        r nn.truncate k2 7
        assert_equal [r hget notifications set] 4
        r del k k2 notifications
    }

    test {Module API native notifications: List PUSH} {
        r nn.rpush k a
        r nn.lpush k a
        assert_equal [r hget notifications rpush] 1
        assert_equal [r hget notifications lpush] 1
        r nn.rpush k b
        r nn.lpush k b
        r nn.rpush k c
        r nn.lpush k c
        assert_equal [r hget notifications rpush] 3
        assert_equal [r hget notifications lpush] 3
        r del k notifications
    }

    test {Module API native notifications: List POP} {
        r nn.lpush k a
        r nn.lpush k b
        r nn.lpush k c
        assert_equal [r hget notifications lpush] 3
        r nn.lpop k
        r nn.rpop k
        assert_equal [r hget notifications lpop] 1
        assert_equal [r hget notifications rpop] 1
        assert_equal [r hget notifications del] ""
        r nn.lpop k
        assert_equal [r hget notifications lpop] 2
        assert_equal [r hget notifications del] 1
        r nn.lpop k
        assert_equal [r hget notifications lpop] 2
        assert_equal [r hget notifications del] 1
        r del k notifications
    }

    test {Module API native notifications: ZADD} {
        r nn.zadd k 10 a
        r nn.zadd k 10 b
        r nn.zadd k 10 c
        # Same element, no notification
        r nn.zadd k 10 c
        assert_equal [r hget notifications zadd] 3
        r del k notifications
    }

    test {Module API native notifications: ZINCRBY} {
        r nn.zincrby k 10 a
        r nn.zincrby k 10 b
        r nn.zincrby k 10 c
        # Same elemant, should be a notification
        r nn.zincrby k 10 c
        assert_equal [r hget notifications zincr] 4
        r del k notifications
    }

    test {Module API native notifications: ZREM} {
        r nn.zadd k 10 a
        r nn.zadd k 10 b
        r nn.zadd k 10 c
        assert_equal [r hget notifications zadd] 3
        r nn.zrem k a
        r nn.zrem k b
        assert_equal [r hget notifications zrem] 2
        # Empty key
        r nn.zrem k2 whatever
        # Non existent element
        r nn.zrem k whatever
        # Last element
        r nn.zrem k c
        assert_equal [r hget notifications zrem] 3
        assert_equal [r hget notifications del] 1
        r del k notifications
    }

    test {Module API native notifications: HSET} {
        r nn.hset k f1 a
        r nn.hset k f2 b
        r nn.hset k f3 c
        # Same elemant, should be a notification
        r nn.hset k f3 c
        assert_equal [r hget notifications hset] 4
        r del k notifications
    }

    test {Module API native notifications: HDEL} {
        r nn.hset k f1 a
        r nn.hset k f2 b
        r nn.hset k f3 c
        # Same elemant, should be a notification
        r nn.hset k f3 c
        assert_equal [r hget notifications hset] 4
        r nn.hdel k f1
        r nn.hdel k f2
        assert_equal [r hget notifications hdel] 2
        # Empty key
        r nn.hdel k2 whatever
        # Non existent element
        r nn.hdel k whatever
        # Last element
        r nn.hdel k f3
        assert_equal [r hget notifications hdel] 3
        assert_equal [r hget notifications del] 1
        r del k notifications
    }
}
