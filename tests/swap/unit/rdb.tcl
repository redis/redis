start_server {overrides {save ""} tags {"swap" "rdb"}} {
    # test {keyspace and rocksdb obj encoding not binded} {
        # r config set hash-max-ziplist-entries 5
        # for {set i 0} {$i < 10} {incr i} {
            # r hset hashbig field_$i val_$i
        # }
        # # saved as hashtable in rocksdb
        # r evict hashbig;
        # assert_equal [r object encoding hashbig] hashtable

        # # decoded as hashtable in rocksdb, loaded as ziplist
        # r config set hash-max-ziplist-entries 512
        # assert_equal [r object encoding hashbig] ziplist

        # # saved as ziplist rocksdb, loaded as ziplist
        # r evict hashbig
        # assert_equal [r object encoding hashbig] ziplist
    # }

    test {rdbsave and rdbload} {
        r config set debug-evict-keys 0

        test {memkey} {
            r flushdb
            r hmset memkey1 a a b b 1 1 2 2 
            r hmset memkey2 a a b b 1 1 2 2 
            r hmset memkey3 a a b b 1 1 2 2 
            r debug reload
            assert_equal [r dbsize] 3
            assert_equal [r hmget memkey1 a b 1 2] {a b 1 2}
        }

        test {wholekey} {
            r flushdb
            r hmset wholekey_hot a a b b 1 1 2 2 
            r hmset wholekey_cold a a b b 1 1 2 2 
            r evict wholekey_cold
            wait_key_evicted r wholekey_cold
            assert_equal [object_meta_len r wholekey_hot] 0
            assert_equal [object_meta_len r wholekey_cold] 0

            r debug reload

            assert_equal [r dbsize] 2
            assert_equal [r hmget wholekey_hot a b 1 2] {a b 1 2}
            assert_equal [r hmget wholekey_cold a b 1 2] {a b 1 2}
        }

        test {bighash} {
            set old_thres [lindex [r config get swap-big-hash-threshold] 1]
            set old_entries [lindex [r config get hash-max-ziplist-entries] 1]
            r config set swap-big-hash-threshold 1
            r config set hash-max-ziplist-entries 1

            # init data
            r flushdb
            r hmset hot a a b b 1 1 2 2 
            r hmset warm a a 1 1
            r evict warm
            r hmset warm b b 2 2
            r hmset cold a a b b 1 1 2 2 
            r evict cold
            assert_equal [object_is_big r warm] 1
            assert_equal [object_is_big r cold] 1
            assert_equal [object_meta_len r hot] 0
            assert_equal [object_meta_len r warm] 2
            assert_equal [object_meta_len r cold] 4
            # reload
            r debug reload
            # check
            assert_equal [object_is_big r warm] 1
            assert_equal [object_is_big r cold] 1
            assert_equal [object_meta_len r hot] 4
            assert_equal [object_meta_len r warm] 4
            assert_equal [object_meta_len r cold] 4

            r config set swap-big-hash-threshold $old_thres
            r config set hash-max-ziplist-entries $old_entries
        }

    }
}
