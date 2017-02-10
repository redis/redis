start_server {tags {"introspection"}} {
    test {TTL and TYPYE do not alter the last access time of a key} {
        r set foo bar
        after 3000
        r ttl foo
        r type foo
        assert {[r object idletime foo] >= 2}
    }

    test {TOUCH alters the last access time of a key} {
        r set foo bar
        after 3000
        r touch foo
        assert {[r object idletime foo] < 2}
    }

    test {TOUCH returns the number of existing keys specified} {
        r flushdb
        r set key1 1
        r set key2 2
        r touch key0 key1 key2 key3
    } 2
}
