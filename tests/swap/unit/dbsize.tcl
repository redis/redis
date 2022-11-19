test "dbsize" {
    start_server {tags {"dbsize - zset"}} {
        r config set swap-debug-evict-keys 0
        r config set swap-evict-step-max-subkeys 1
        test "dbsize zset" {
            r zadd zset 10 a 20 b  
            r evict zset
            assert_equal [r dbsize] 1 
            r evict zset 
            assert_equal [r dbsize] 1
            r zrange zset 0 -1 
            assert_equal [r dbsize] 1
            r evict zset 
            assert_equal [r dbsize] 1
            r evict zset 
            assert_equal [r dbsize] 1
        }
        
    }


    start_server {tags {"dbsize hash"}} {
        r config set swap-debug-evict-keys 0
        r config set swap-evict-step-max-subkeys 1
        test "dbsize hash" {
            r hset hash a 1 b 2 
            r evict hash
            assert_equal [r dbsize] 1 
            r evict hash 
            assert_equal [r dbsize] 1
            r hgetall  hash
            assert_equal [r dbsize] 1
            r evict hash 
            assert_equal [r dbsize] 1
            r evict hash 
            assert_equal [r dbsize] 1
        }
    }

    start_server {tags {"dbsize set"}} {
        r config set swap-debug-evict-keys 0
        r config set swap-evict-step-max-subkeys 1
        test "dbsize set" {
            r sadd set a b
            r evict set
            assert_equal [r dbsize] 1 
            r evict set 
            assert_equal [r dbsize] 1
            r SMEMBERS set
            assert_equal [r dbsize] 1
            r evict set 
            assert_equal [r dbsize] 1
            r evict set 
            assert_equal [r dbsize] 1
        }
    }

    start_server {tags {"dbsize set"}} {
        r config set swap-debug-evict-keys 0
        r config set swap-evict-step-max-subkeys 1
        test "dbsize set" {
            r sadd set a b
            r evict set
            assert_equal [r dbsize] 1 
            r evict set 
            assert_equal [r dbsize] 1
            r SMEMBERS set
            assert_equal [r dbsize] 1
            r evict set 
            assert_equal [r dbsize] 1
            r evict set 
            assert_equal [r dbsize] 1
        }
    }

    start_server {tags {"dbsize list"}} {
        r config set swap-debug-evict-keys 0
        r config set swap-evict-step-max-subkeys 1
        test "dbsize list" {
            r LPUSH list a b
            r evict list
            assert_equal [r dbsize] 1 
            r evict list 
            assert_equal [r dbsize] 1
            puts [r lrange list 0 -1]
            assert_equal [r dbsize] 1
            r evict list 
            assert_equal [r dbsize] 1
            r evict list 
            assert_equal [r dbsize] 1
        }
    }
}
