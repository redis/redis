start_server {tags "bighash"} {
    r config set debug-evict-keys 0
    r config set swap-big-hash-threshold 1

    test {swapin entire hash} {
        r hmset hash1 a a b b 1 1 2 2
        r evict hash1
        wait_keys_evicted r
        r hgetall hash1
        r del hash1
    }

    test {swapin specific hash fields} {
        r hmset hash2 a a b b 1 1 2 2
        r evict hash2
        wait_keys_evicted r
        # cold - not exists fields
        assert_equal [r hmget hash2 not_exist 99] {{} {}}
        # cold - exists fields
        assert_equal [r hmget hash2 a 1] {a 1}
        # warm - not exists fields
        assert_equal [r hmget hash2 not_exist 99] {{} {}}
        # warm - exists fields
        assert_equal [r hmget hash2 b 2] {b 2}
        # hot - not exists fields
        assert_equal [r hmget hash2 not_exist 99] {{} {}}
        # hot - exist fields
        assert_equal [r hmget hash2 a 1] {a 1}
        r del hash2
    }

    test {IN.meta} {
        r hmset hash3 a a b b 1 1 2 2
        r evict hash3
        wait_keys_evicted r
        assert_equal [r hlen hash3] 4
        assert_equal [llength [r hkeys hash3]] 4
        assert_equal [r hlen hash3] 4
        r hdel hash3 1
        assert_equal [r hlen hash3] 3
        r hdel hash3 a b 1 2
        assert_equal [r hlen hash3] 0
        r del hash3
    }

    test {IN.del} {
        r hmset hash4 a a b b 1 1 2 2
        r del hash4
        assert_equal [r exists hash4] 0

        r hmset hash5 a a b b 1 1 2 2
        r evict hash5
        wait_keys_evicted r
        r del hash5
        assert_equal [r exists hash5] 0
        r del hash4 hash5
    }

    test {active expire cold hash} {
        r hmset hash6 a a b b 1 1 2 2
        r evict hash6
        wait_keys_evicted r
        r pexpire hash6 10
        after 100
        assert_equal [r ttl hash6] -2
        r del hash6
    }

    test {active expire hot hash} {
        r hmset hash7 a a b b 1 1 2 2
        r pexpire hash7 10
        after 100
        assert_equal [r ttl hash7] -2
        r del hash7
    }

    test {lazy expire cold hash} {
        r debug set-active-expire 0
        r hmset hash8 a a b b 1 1 2 2
        r evict hash8
        wait_keys_evicted r
        r pexpire hash8 10
        after 100
        assert_equal [r ttl hash8] -2
        r del hash8
    }

    test {lazy expire hot hash} {
        r debug set-active-expire 0
        r hmset hash9 a a b b 1 1 2 2
        r pexpire hash9 10
        after 100
        assert_equal [r ttl hash9] -2
        assert_equal [r del hash9] 0
        r debug set-active-expire 1
    }

    test {lazy del obseletes rocksdb data} {
        r hmset hash11 a a b b 1 1 2 2 
        r evict hash11
        wait_keys_evicted r
        set meta_version [object_meta_version r hash11]
        set old_b_encode [r swap encode-key h $meta_version hash11 b]
        r del hash11
        assert_equal [r exists hash11] 0
        assert_equal [r hget hash11 a] {}
        assert {[r swap rio-get $old_b_encode] != {}}
    }

    test {wholekey bighash transform} {

        r config set swap-big-hash-threshold 1
        r hmset hash10 a a b b 1 1 2 2 
        r evict hash10
        assert_equal [object_is_big r hash10] 1
        wait_keys_evicted r
        r config set swap-big-hash-threshold 256k
        assert_equal [llength [r hkeys hash10]] 4;
        assert_equal [object_is_big r hash10] 1
        r evict hash10
        wait_keys_evicted r
        assert_equal [object_is_big r hash10] 0
        assert_equal [llength  [r hvals hash10]] 4
        assert_equal [object_is_big r hash10] 0
        r del hash10
    }

}
