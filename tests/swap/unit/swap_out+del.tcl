start_server {tags {"swap string"}} {
    r config set swap-debug-evict-keys 0
    test {swap out string} {
        r set k v
        r swap.evict k
        wait_key_cold r k
    }

    test {swap in string} {
        r del k
        assert [keyspace_is_empty r]
    }
}


start_server {tags {"swap  small hash"}} {
    r config set swap-debug-evict-keys 0
    test {swap out hash} {
        r hset h k v
        r swap.evict h
        wait_key_cold r h
    }

    test {swap in hash} {
        r del h
        assert [keyspace_is_empty r]
    }
} 

