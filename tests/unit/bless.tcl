start_server {tags {"bless"}} {
    test {BLESS SET/GET/COUNT basics} {
        r flushall
        r set k v
        # explicit NO-EVICT ON protects; reply 1 on change, 0 on no-op
        assert_equal 1 [r bless set k no-evict on]
        assert_equal 0 [r bless set k no-evict on]
        assert_equal {NO-EVICT ON} [r bless get k]
        assert_equal 1 [r bless count]
        # SET with no arg defaults to ON
        r set k2 v
        assert_equal 1 [r bless set k2]
        assert_equal 2 [r bless count]
        # OFF clears the protection
        assert_equal 1 [r bless set k no-evict off]
        assert_equal {NO-EVICT OFF} [r bless get k]
        assert_equal 1 [r bless count]
    }

    test {BLESS SET/GET on a missing key errors} {
        r flushall
        assert_error {*no such key*} {r bless set nope no-evict on}
        assert_error {*no such key*} {r bless get nope}
    }

    test {BLESS survives DEBUG RELOAD (RDB round-trip)} {
        r flushall
        r set a 1; r set b 2; r set c 3
        r bless set a no-evict on
        r bless set b no-evict on
        assert_equal 2 [r bless count]
        r debug reload
        # index rebuilt on load; levels and values intact
        assert_equal 2 [r bless count]
        assert_equal {NO-EVICT ON}  [r bless get a]
        assert_equal {NO-EVICT ON}  [r bless get b]
        assert_equal {NO-EVICT OFF} [r bless get c]
        assert_equal 1 [r get a]
    }

    test {BLESS survives DUMP/RESTORE} {
        r flushall
        r set a hello
        r bless set a no-evict on
        set d [r dump a]
        r del a
        assert_equal 0 [r bless count]
        r restore a 0 $d
        assert_equal {NO-EVICT ON} [r bless get a]
        assert_equal 1 [r bless count]
    }

    test {BLESS and TTL coexist across DEBUG RELOAD} {
        r flushall
        r set a 1
        r expire a 10000
        r bless set a no-evict on
        r debug reload
        assert_equal {NO-EVICT ON} [r bless get a]
        assert {[r ttl a] > 0}
    }

    test {BLESS OFF is the reset sentinel and is not persisted} {
        r flushall
        r set a 1
        r bless set a no-evict on
        r bless set a no-evict off
        assert_equal 0 [r bless count]
        r debug reload
        assert_equal 0 [r bless count]
        assert_equal {NO-EVICT OFF} [r bless get a]
    }
}

start_server {tags {"bless" "maxmemory" "external:skip"}} {
    test {BLESS NO-EVICT key is not evicted under memory pressure} {
        r flushall
        r config set maxmemory 0
        r config set maxmemory-policy allkeys-random
        r set precious [string repeat x 2000]
        r bless set precious no-evict on
        set used [s used_memory]
        r config set maxmemory [expr {$used + 3000000}]
        # flood ~8MB of data into ~3MB of headroom -> forces eviction
        for {set j 0} {$j < 4000} {incr j} {
            r set flood:$j [string repeat y 2000]
        }
        assert {[s evicted_keys] > 0}
        assert_equal 1 [r exists precious]
        assert_equal {NO-EVICT ON} [r bless get precious]
        r config set maxmemory 0
    }

    test {All-blessed keyspace gives a clean OOM (no hang); unbless relieves it} {
        r flushall
        r config set maxmemory 0
        r config set maxmemory-policy allkeys-random
        for {set j 0} {$j < 500} {incr j} {
            r set b:$j [string repeat y 1000]
            r bless set b:$j no-evict on
        }
        assert_equal 500 [r bless count]
        set used [s used_memory]
        r config set maxmemory [expr {$used + 50000}]
        # every key is protected -> eviction can free nothing -> clean OOM, no hang
        assert_error {*OOM*} {r set toobig [string repeat z 200000]}
        # remove all protections -> eviction can proceed again
        for {set j 0} {$j < 500} {incr j} { r bless set b:$j no-evict off }
        assert_equal 0 [r bless count]
        r set toobig [string repeat z 100000]
        assert_equal 1 [r exists toobig]
        r config set maxmemory 0
    }
}
