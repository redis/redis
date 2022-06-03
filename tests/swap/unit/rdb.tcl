start_server {overrides {save ""} tags {"swap" "rdb"}} {
    test {keyspace and rocksdb obj encoding not binded} {
        r config set hash-max-ziplist-entries 5
        for {set i 0} {$i < 10} {incr i} {
            r hset hashbig field_$i val_$i
        }
        # saved as hashtable in rocksdb
        r evict hashbig;
        assert_equal [r object encoding hashbig] hashtable

        # decoded as hashtable in rocksdb, loaded as ziplist
        r config set hash-max-ziplist-entries 512
        assert_equal [r object encoding hashbig] ziplist

        # saved as ziplist rocksdb, loaded as ziplist
        r evict hashbig
        assert_equal [r object encoding hashbig] ziplist
    }
}
