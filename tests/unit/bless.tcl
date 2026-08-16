start_server {tags {"bless"}} {
    test {BLESS SET/GET/COUNT basics} {
        r flushall
        r set k v
        # explicit NO-EVICT protects; reply 1 on change, 0 on no-op
        assert_equal 1 [r bless set k no-evict]
        assert_equal 0 [r bless set k no-evict]
        assert_equal {NO-EVICT} [r bless get k]
        assert_equal 1 [r bless count]
        # a second key, using the default level (NO-EVICT)
        r set k2 v
        assert_equal 1 [r bless set k2]
        assert_equal {NO-EVICT} [r bless get k2]
        assert_equal 2 [r bless count]
        # NONE clears the protection
        assert_equal 1 [r bless set k none]
        assert_equal {NONE} [r bless get k]
        assert_equal 1 [r bless count]
    }

    test {BLESS SET/GET on a missing key errors} {
        r flushall
        assert_error {*no such key*} {r bless set nope no-evict}
        assert_error {*no such key*} {r bless get nope}
    }

    test {BLESS SET level defaults to NO-EVICT; bad level errors} {
        r flushall
        r set k v
        # bare SET defaults to NO-EVICT
        assert_equal 1 [r bless set k]
        assert_equal {NO-EVICT} [r bless get k]
        # unknown level -> syntax error
        assert_error {*syntax*} {r bless set k bogus}
    }

    test {BLESS LIST returns keys blessed at/above the given level (default NO-EVICT)} {
        r flushall
        r set a 1; r set b 2; r set c 3
        r bless set a no-evict
        r bless set b no-evict
        assert_equal [lsort {a b}] [lsort [r bless list]]
        assert_equal [lsort {a b}] [lsort [r bless list no-evict]]
        # NONE removes the key from the list
        r bless set a none
        assert_equal {b} [r bless list]
        assert_error {*syntax*} {r bless list bogus}
    }

    test {BLESS survives value overwrite (all types); COUNT/LIST stay consistent} {
        r flushall
        # string overwrite via SET
        r set k v1
        r bless set k no-evict
        r set k v2
        assert_equal {NO-EVICT} [r bless get k]
        assert_equal 1 [r bless count]
        assert_equal {k} [r bless list]
        # overwrite that changes the type (hash -> string) keeps the blessing
        r hset h f v
        r bless set h no-evict
        r set h nowstring
        assert_equal {NO-EVICT} [r bless get h]
        # in-place modification keeps it too
        r rpush lst a
        r bless set lst no-evict
        r rpush lst b c
        assert_equal {NO-EVICT} [r bless get lst]
        assert_equal 3 [r bless count]
        # NONE and key removal are the only things that clear it
        r bless set k none
        assert_equal {NONE} [r bless get k]
        r del h
        r set h x
        assert_equal {NONE} [r bless get h]
        assert_equal 1 [r bless count]
    }

    test {BLESS survives DEBUG RELOAD (RDB round-trip)} {
        r flushall
        r set a 1; r set b 2; r set c 3
        r bless set a no-evict
        r bless set b no-evict
        assert_equal 2 [r bless count]
        r debug reload
        # index rebuilt on load; levels and values intact
        assert_equal 2 [r bless count]
        assert_equal {NO-EVICT} [r bless get a]
        assert_equal {NO-EVICT} [r bless get b]
        assert_equal {NONE}     [r bless get c]
        assert_equal 1 [r get a]
    }

    test {BLESS survives DUMP/RESTORE} {
        r flushall
        r set a hello
        r bless set a no-evict
        set d [r dump a]
        r del a
        assert_equal 0 [r bless count]
        r restore a 0 $d
        assert_equal {NO-EVICT} [r bless get a]
        assert_equal 1 [r bless count]
    }

    test {BLESS and TTL coexist across DEBUG RELOAD} {
        r flushall
        r set a 1
        r expire a 10000
        r bless set a no-evict
        r debug reload
        assert_equal {NO-EVICT} [r bless get a]
        assert {[r ttl a] > 0}
    }

    test {BLESS NONE is the reset sentinel and is not persisted} {
        r flushall
        r set a 1
        r bless set a no-evict
        r bless set a none
        assert_equal 0 [r bless count]
        r debug reload
        assert_equal 0 [r bless count]
        assert_equal {NONE} [r bless get a]
    }
}

start_server {tags {"bless" "maxmemory" "external:skip"}} {
    test {BLESS NO-EVICT key is not evicted under memory pressure} {
        r flushall
        r config set maxmemory 0
        r config set maxmemory-policy allkeys-random
        r set precious [string repeat x 2000]
        r bless set precious no-evict
        set used [s used_memory]
        r config set maxmemory [expr {$used + 3000000}]
        # flood ~8MB of data into ~3MB of headroom -> forces eviction
        for {set j 0} {$j < 4000} {incr j} {
            r set flood:$j [string repeat y 2000]
        }
        assert {[s evicted_keys] > 0}
        assert_equal 1 [r exists precious]
        assert_equal {NO-EVICT} [r bless get precious]
        r config set maxmemory 0
    }

    test {All-blessed keyspace gives a clean OOM (no hang); unbless relieves it} {
        r flushall
        r config set maxmemory 0
        r config set maxmemory-policy allkeys-random
        for {set j 0} {$j < 500} {incr j} {
            r set b:$j [string repeat y 1000]
            r bless set b:$j no-evict
        }
        assert_equal 500 [r bless count]
        set used [s used_memory]
        r config set maxmemory [expr {$used + 50000}]
        # every key is protected -> eviction can free nothing -> clean OOM, no hang
        assert_error {*OOM*} {r set toobig [string repeat z 200000]}
        # remove all protections -> eviction can proceed again
        for {set j 0} {$j < 500} {incr j} { r bless set b:$j none }
        assert_equal 0 [r bless count]
        r set toobig [string repeat z 100000]
        assert_equal 1 [r exists toobig]
        r config set maxmemory 0
    }

    test {BLESS SET is not DENYOOM: works under memory pressure (like EXPIRE)} {
        r flushall
        r config set maxmemory 0
        r config set maxmemory-policy noeviction
        r set important v
        r set other v
        # force OOM: cap maxmemory below current usage
        set used [s used_memory]
        r config set maxmemory [expr {$used - 100000}]
        # a real DENYOOM write is rejected here...
        assert_error {*OOM*} {r set grow [string repeat z 1000]}
        # ...but BLESS SET is not DENYOOM (its footprint is one keymeta slot,
        # like EXPIRE), so both protecting and unblessing work under OOM.
        assert_equal 1 [r bless set important no-evict]
        assert_equal {NO-EVICT} [r bless get important]
        assert_equal 1 [r bless set important none]
        assert_equal {NONE} [r bless get important]
        r config set maxmemory 0
    }
}
