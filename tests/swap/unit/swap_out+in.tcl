start_server {tags {"swap string"}} {
    r config set swap-debug-evict-keys 0
    test {swap out string} {
        r set k v
        r evict k
        wait_key_cold r k
    }

    test {swap in string} {
        assert_equal [r get k] v
        assert ![object_is_cold r k]
    }
}


start_server {tags {"swap  small hash"}} {
    r config set swap-debug-evict-keys 0
    test {swap out hash} {

        r hset h k v
        r evict h
        wait_key_cold r h
    }

    test {swap in hash} {
        assert_equal [r hget h k] v
        assert ![object_is_cold r h]
    }
} 

