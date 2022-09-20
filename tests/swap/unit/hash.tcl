start_server {tags "bighash"} {
    r config set debug-evict-keys 0
    r config set swap-big-hash-threshold 1

    test {swapin entire hash} {
        r hmset hash1 a a b b 1 1 2 2
        r evict hash1
        wait_key_cold r hash1
        assert_equal [lsort [r hgetall hash1]] {1 1 2 2 a a b b}
        r del hash1
    }

    test {swapin specific hash fields} {
        r hmset hash2 a a b b 1 1 2 2
        r evict hash2
        wait_key_cold r hash2
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
        wait_key_cold r hash3
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
        wait_key_cold r hash5
        r del hash5
        assert_equal [r exists hash5] 0
        r del hash4 hash5
    }

    test {active expire cold hash} {
        r hmset hash6 a a b b 1 1 2 2
        r evict hash6
        wait_key_cold r hash6
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
        wait_key_cold r hash8
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
        wait_key_cold r hash11
        set meta_raw [r swap rio-get meta [r swap encode-meta-key hash11]]
        assert {$meta_raw != {{}}}
        r del hash11
        assert_equal [r exists hash11] 0
        assert_equal [r hget hash11 a] {}
        set meta_raw [r swap rio-get meta [r swap encode-meta-key hash11]]
        assert {$meta_raw == {{}}}
    }

    test {evict clean hash triggers no swap} {
        r hmset hash10 a a b b 1 1 2 2 
        r evict hash10
        wait_key_cold r hash10
        set old_swap_out_count [get_info_property r Swaps swap_OUT count]
        r hgetall hash10
        r evict hash10
        # evict clean hash triggers no swapout
        assert_equal $old_swap_out_count [get_info_property r Swaps swap_OUT count]

        assert_equal [llength [r hkeys hash10]] 4
        r evict hash10
        assert_equal [llength  [r hvals hash10]] 4
        r del hash10
    }

    test {bighash dirty & meta} {
        set old_swap_threshold [lindex [r config get swap-big-hash-threshold] 1]
        set old_swap_max_subkeys [lindex [r config get swap-evict-step-max-subkeys] 1]

        r config set swap-big-hash-threshold 1
        r config set swap-evict-step-max-subkeys 2

        # bighash are initialized as dirty
        r hmset hash11 a a b b 1 1 2 2 
        assert [object_is_dirty r hash11]

        # dirty bighash partial evict remains dirty
        r evict hash11
        wait_key_warm r hash11
        assert [object_is_dirty r hash11]
        assert_equal [object_meta_len r hash11] 2

        # dirty bighash all evict
        r evict hash11
        wait_key_cold r hash11
        assert ![object_is_dirty r hash11]
        assert_equal [object_meta_len r hash11] 4

        # cold bighash turns clean when swapin
        assert_equal [r hmget hash11 a 1] {a 1}
        assert ![object_is_dirty r hash11]
        assert_equal [object_meta_len r hash11] 2

        # clean bighash all swapin remains clean
        assert_equal [r hmget hash11 b 2] {b 2}
        assert ![object_is_dirty r hash11]
        # all-swapin bighash meta remains
        assert_equal [object_meta_len r hash11] 0
        assert_equal [r hlen hash11] 4

        # clean bighash swapout does not triggers swap
        set orig_swap_out_count [get_info_property r swaps swap_OUT count]

        r evict hash11
        assert ![object_is_dirty r hash11]
        assert_equal $orig_swap_out_count [get_info_property r Swaps swap_OUT count]

        # clean bighash swapout does not triggers swap
        assert_equal [r hmget hash11 b 2] {b 2}
        assert ![object_is_dirty r hash11]
        assert_equal $orig_swap_out_count [get_info_property r Swaps swap_OUT count]

        # clean bighash swapout fields can be retrieved from rocksdb
        assert_equal [r hget hash11 1] {1}

        # modify bighash makes bighash dirty
        r hmset hash11 a A
        assert [object_is_dirty r hash11]

        # dirty bighash evict triggers swapout
        after 200
        assert_equal [r evict hash11] 1
        after 200
        assert [object_is_dirty r hash11]
        assert_equal [r hlen hash11] 4
        assert {[get_info_property r Swaps swap_OUT count] > $orig_swap_out_count}

        r config set swap-big-hash-threshold $old_swap_threshold
        r config set swap-evict-step-max-subkeys $old_swap_max_subkeys
    }

    test {bighash hdel & del} {
        r hmset hash12 a a 1 1
        assert_equal [r evict hash12] 1
        wait_key_cold r hash12
        assert_equal [object_meta_len r hash12] 2
        r hdel hash12 a
        assert_equal [object_meta_len r hash12] 1
        r hdel hash12 1
        assert {[rio_get_data r hash12 a] eq {}}
        assert {[rio_get_data r hash12 1] eq {}}
        assert {[rio_get_meta r hash12] eq {}}
        catch {r swap object hash12} err
        assert_match "*ERR no such key*" $err
    }

    test {hdel warm key} {
        r hmset hash13 a a b b c c 1 1 2 2 3 3 
        assert_equal [r evict hash13] 1
        wait_key_cold r hash13
        assert [object_is_big r hash13]
        assert_equal [r hmget hash13 a b 1 2] {a b 1 2}
        assert_equal [object_meta_len r hash13] 2
        assert_equal [r hdel hash13 a 1 c 99] 3
        assert_equal [r hlen hash13] 3
        assert_equal [object_meta_len r hash13] 1
        assert_equal [r hdel hash13 b 3 z 99] 2
        assert_equal [object_meta_len r hash13] 0
        assert_equal [r hlen hash13] 1
        assert_equal [r del hash13] 1
    }

    test {big hash deletes other key by mistake} {
        r hmset hash_aa foo bar        
        # hash_aa is encoded as: h|1|7hash_aa
        r evict hash_aa
        wait_key_cold r hash_aa        
        r hmset hash_a foo bar
        # hash_a is encoded as: h|2|6hash_a
        r evict hash_a
        wait_key_cold r hash_a
        # if hash subkey is encoded enc_type|version|keylen|key|subkey
        # (commit 72322eea3), then delete range is [ h|1, h|2|7|hash_aa ),
        # which includes h|2|6|hash_a, so subkey of hash_a will be deleted
        # by mistake.
        r del hash_aa
        assert_equal [r hkeys hash_a] {foo}
    }
    test {hdel & reload} {
        r hmset hash14 a a b b c c 1 1 2 2 3 3 
        assert_equal [r evict hash14] 1
        wait_key_cold r hash14
        # evict cold
        r evict hash14
        wait_key_cold r hash14
        # reload cold
        r debug reload
        assert_equal [r hmget hash14 a b 1 2] {a b 1 2}
        # hdel non-existing field does not modify hlen, but meta gets deleted
        r hdel hash14 x
        assert_equal [r hlen hash14] 6
        # reload warm
        r debug reload
        assert_equal [r hmget hash14 a b c 1 2 3] {a b c 1 2 3}
        assert_equal [r hlen hash14] 6
        # reload hot
        r debug reload
        assert_equal [r hlen hash14] 6
    }

    test {hdel cold field untill hash empty} {
        r hset hash15 a a
        r evict hash15
        r hget hash15 b
        r hdel hash15 a
        catch {r swap object hash15} e
        assert_match {*no such key*} $e
    }
}

start_server {tags {"evict big hash, check hlen"}} {
    test {check meta on hash evict} {
        r config set debug-evict-keys 0
        r config set swap-big-hash-threshold 1
        r config set swap-evict-step-max-subkeys 2
        r hmset myhash a a b b c c d d e e f f g g h h
        assert_equal [r hlen myhash] 8
        assert_equal [r evict myhash] 1
        after 100
        assert_equal [r hlen myhash] 8
        after 100
        assert_equal [r evict myhash] 1
        after 100
        assert_equal [r hlen myhash] 8
        after 100
    }
} 


test {big hash check reload1} {
    start_server {tags {"evict big hash, check reload"}} {
        r config set debug-evict-keys 0
        r config set swap-big-hash-threshold 1
        r config set swap-evict-step-max-subkeys 2
        r hmset myhash a a b b c c d d e e f f g g h h
        assert_equal [r hlen myhash] 8
        assert_equal [r evict myhash] 1
        for {set j 0} { $j < 3} {incr j} {
            after 100
            assert_equal [r evict myhash] 1

        } 
        after 1000
        assert_equal [r hlen myhash] 8
        r hmget myhash a b c d e f g
        assert_equal [r hlen myhash] 8
        after 1000
        r debug reload 
        assert_equal [r hlen myhash] 8
    }
}

test {big hash random get check reload} {
    start_server {tags {"evict big hash, check reload"}} {
        r config set debug-evict-keys 0
        r config set swap-big-hash-threshold 1
        r config set swap-evict-step-max-subkeys 2

        set test_count 1000
        set hlen 100
        for {set i 0} {$i < $test_count} {incr i} {
            for {set j 0} {$j < $hlen} {incr j} {
                r hset myhash $j [randomValue]
            } 
            set evict_count  [randomInt [expr $hlen/2]]
            for {set j 0} {$j < $evict_count} {incr j} {
                r evict  myhash
            }
            set get_count [randomInt $hlen]
            for {set j 0} {$j < $get_count} {incr j} {
                r hget myhash $j 
            }           
            assert_equal [r hlen myhash] $hlen
            r debug reload
        }

    }
}


test {two big hash check reload1} {
    start_server {tags {"evict big hash, check reload"}} {
        r config set debug-evict-keys 0
        r config set swap-big-hash-threshold 1
        r config set swap-evict-step-max-subkeys 2
        r config set save ""
        r hmset myhash a a b b c c d d e e f f g g h h
        assert_equal [r hlen myhash] 8
        r hmset myhash1 a a b b c c
        assert_equal [r hlen myhash1] 3
        assert_equal [r evict myhash1] 1
        for {set j 0} { $j < 4} {incr j} {
            after 100
            assert_equal [r evict myhash] 1
        } 
        after 1000
        puts [r debug reload]
        assert_equal [r hlen myhash] 8
        assert_equal [r hlen myhash1] 3
    }
}

