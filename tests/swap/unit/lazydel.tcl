start_server {tags "expire"} {
    r config set debug-evict-keys 0
    r config set swap-big-hash-threshold 1

    test {filter lazy deleted cold key} {
        r hmset filter00 a a b b 1 1 2 2 
        r evict filter00
        assert_equal [r hlen filter00] 4
        assert {[r swap rio-scan h] != {}}
        r del filter00
        assert {[r swap rio-scan h] == {}}
    }

    test {filter lazy deleted warm key} {
        r hmset filter01 a a b b 1 1 2 2 
        r evict filter01
        r hmget filter01 a 1
        assert_equal [r hlen filter01] 4
        r del filter01
        assert_equal [r hlen filter01] 0
        assert {[r swap rio-scan h] == {}}
    }

    test {filter lazy deleted hot key} {
        r hmset filter02 a a b b 1 1 2 2 
        r evict filter02
        r hkeys filter02
        assert_equal [r hlen filter02] 4
        assert_equal [object_meta_len r filter02] 0
        r del filter02
        assert_equal [r hlen filter02] 0
        assert {[r swap rio-scan h] == {}}
    }

    test {filter lazy deleted bighash, now wholekey} {
        r hmset filter03 a a b b 1 1 2 2 
        r evict filter03
        r hkeys filter03
        assert_equal [r hlen filter03] 4
        assert_equal [object_meta_len r filter03] 0
        r del filter03
        r set filter03 foo
        r evict filter03
        wait_key_cold r filter03
        assert {[r swap rio-scan h] == {}}
        assert_equal [r get filter03] foo
        r del filter03
        assert {[r swap rio-scan ""] == {}}
    }

    test {filter obselete subkeys} {
        r hmset filter04 a a b b 1 1 2 2 
        r evict filter04
        r hkeys filter04
        assert_equal [r hlen filter04] 4
        assert_equal [object_meta_len r filter04] 0
        r del filter04
        assert_equal [r hlen filter04] 0
        r hmset filter04 a A b B 1 11 2 22
        r evict filter04
        assert_equal [r hmget filter04 a 1] {A 11}
        assert_equal [llength [r swap rio-scan h]] 4
        assert_equal [object_meta_len r filter04] 2
        assert_equal [llength [r swap rio-scan h]] 4
        assert_equal [r hmget filter04 b 2] {B 22}
        r del filter04
        assert_equal [llength [r swap rio-scan h]] 0
    }

    test {filter expired bighash} {
        r hmset filter05 a a b b 1 1 2 2 
        r evict filter05
        assert_equal [r hlen filter05] 4
        assert_equal [object_meta_len r filter05] 4
        r pexpire filter05 100
        after 500
        assert [keyspace_is_empty r]
        r hmset filter05 a A b B
        assert_equal [llength [r swap rio-scan h]] 0
        r del filter05
        assert_equal [llength [r swap rio-scan h]] 0
    }

    # test {filter transformed wholekey} {
        # set old_thres [r config get swap-big-hash-threshold]
        # r config set swap-big-hash-threshold 256k

        # r hmset filter06 a a b b 1 1 2 2 
        # r evict filter06
        # wait_key_cold r filter06
        # assert ![object_is_big r filter06]
        # assert {[r swap rio-get [r swap encode-key H filter06]] != {}}

        # r config set swap-big-hash-threshold 1
        # r hkeys filter06
        # r evict filter06
        # assert [object_is_big r filter06]
        # assert {[r swap rio-get [r swap encode-key H filter06]] == {}}

        # r del filter06
        # assert_equal [llength [r swap rio-scan ""]] 0

        # r config set swap-big-hash-threshold $old_thres
    # }
}

