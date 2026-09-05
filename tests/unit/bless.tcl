start_server {tags {"bless"}} {
    test {BLESS SET/GET/LIST basics} {
        r flushall
        r set k v
        # SET NO-EVICT protects; reply 1 on change, 0 on no-op
        assert_equal 1 [r bless set k no-evict]
        assert_equal 0 [r bless set k no-evict]
        assert_equal {NO-EVICT} [r bless get k]
        assert_equal 1 [llength [r bless list no-evict]]
        # a second key
        r set k2 v
        assert_equal 1 [r bless set k2 no-evict]
        assert_equal {NO-EVICT} [r bless get k2]
        assert_equal 2 [llength [r bless list no-evict]]
        # CLEAR removes the protection
        assert_equal 1 [r bless clear k no-evict]
        assert_equal {} [r bless get k]
        assert_equal 1 [llength [r bless list no-evict]]
    }

    test {BLESS SET/GET/CLEAR on a missing key errors} {
        r flushall
        assert_error {*no such key*} {r bless set nope no-evict}
        assert_error {*no such key*} {r bless clear nope no-evict}
        assert_error {*no such key*} {r bless get nope}
    }

    test {BLESS SET/CLEAR require a flag; unknown/unsupported flags error} {
        r flushall
        r set k v
        # at least one flag is required
        assert_error {*wrong number*} {r bless set k}
        assert_error {*wrong number*} {r bless clear k}
        # unknown or not-yet-supported flags -> syntax error
        assert_error {*syntax*} {r bless set k bogus}
        assert_error {*syntax*} {r bless set k none}
        # exactly one flag token (arity 4); extra tokens are an arity error
        assert_error {*wrong number*} {r bless set k no-evict no-evict}
        assert_equal 1 [r bless set k no-evict]
        assert_equal {NO-EVICT} [r bless get k]
    }

    test {BLESS LIST returns keys with the given flag; the flag is required} {
        r flushall
        r set a 1; r set b 2; r set c 3
        r bless set a no-evict
        r bless set b no-evict
        assert_equal [lsort {a b}] [lsort [r bless list no-evict]]
        # CLEAR removes the key from the list
        r bless clear a no-evict
        assert_equal {b} [r bless list no-evict]
        # the flag is required (no default) -> a bare LIST is an arity error
        assert_error {*wrong number*} {r bless list}
        # LIST accepts only NO-EVICT; NONE/junk are a syntax error
        assert_error {*syntax*} {r bless list bogus}
        assert_error {*syntax*} {r bless list none}
    }

    test {BLESS survives value overwrite (all types); LIST stays consistent} {
        r flushall
        # string overwrite via SET
        r set k v1
        r bless set k no-evict
        r set k v2
        assert_equal {NO-EVICT} [r bless get k]
        assert_equal 1 [llength [r bless list no-evict]]
        assert_equal {k} [r bless list no-evict]
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
        assert_equal 3 [llength [r bless list no-evict]]
        # CLEAR and key removal are the only things that clear it
        r bless clear k no-evict
        assert_equal {} [r bless get k]
        r del h
        r set h x
        assert_equal {} [r bless get h]
        assert_equal 1 [llength [r bless list no-evict]]
    }

    test {SET keeps blessing; DEL+SET clears it} {
        r flushall
        # SET, BLESS, SET  -> blessing kept across the overwrite
        r set k v1
        r bless set k no-evict
        r set k v2
        assert_equal {NO-EVICT} [r bless get k]
        assert_equal 1 [llength [r bless list no-evict]]
        # SET, DEL, SET  -> key removal clears the blessing; the recreated key is plain
        r del k
        r set k v3
        assert_equal {} [r bless get k]
        assert_equal 0 [llength [r bless list no-evict]]
    }

    test {MOVE transfers the blessing and leaves no ghost in the source index} {
        r flushall
        r set k v
        r bless set k no-evict
        assert_equal 1 [llength [r bless list no-evict]]
        r move k 10
        # source DB (9): key gone, index has no ghost entry
        assert_equal 0 [r exists k]
        assert_equal 0 [llength [r bless list no-evict]]
        assert_equal {} [r bless list no-evict]
        # destination DB (10): key present and still blessed
        r select 10
        assert_equal {NO-EVICT} [r bless get k]
        assert_equal 1 [llength [r bless list no-evict]]
        r flushall
        r select 9
    } {OK} {cluster:skip}

    test {BLESS survives DEBUG RELOAD (RDB round-trip)} {
        r flushall
        r set a 1; r set b 2; r set c 3
        r bless set a no-evict
        r bless set b no-evict
        assert_equal 2 [llength [r bless list no-evict]]
        r debug reload
        # index rebuilt on load; flags and values intact
        assert_equal 2 [llength [r bless list no-evict]]
        assert_equal {NO-EVICT} [r bless get a]
        assert_equal {NO-EVICT} [r bless get b]
        assert_equal {}         [r bless get c]
        assert_equal 1 [r get a]
    } {} {needs:debug}

    test {DUMP/RESTORE does not carry the blessing} {
        r flushall
        # The blessing is a local attribute, not part of the key's data, so a
        # DUMP payload never carries it - RESTORE brings back a plain key.
        r set a hello
        r bless set a no-evict
        set d [r dump a]
        r del a
        r restore a 0 $d
        assert_equal {} [r bless get a]
        assert_equal 0 [llength [r bless list no-evict]]
    }

    test {RESTORE REPLACE keeps the destination's blessing (payload carries none)} {
        r flushall
        # Payloads never carry a blessing, so RESTORE REPLACE over a blessed
        # destination keeps the destination's blessing.
        r set plain pv
        set d [r dump plain]
        r set k old
        r bless set k no-evict
        r restore k 0 $d replace
        assert_equal {NO-EVICT} [r bless get k]
        assert_equal 1 [r exists k]
        # over an unblessed destination -> stays unblessed
        r set d2 old
        r restore d2 0 $d replace
        assert_equal {} [r bless get d2]
    }

    test {redis-check-rdb accepts an RDB that contains a blessed key} {
        r flushall
        r set plain v
        r set precious v
        r bless set precious no-evict
        r save
        set rdb [file join [lindex [r config get dir] 1] dump.rdb]
        # exec throws if the checker exits non-zero (i.e. reports corruption);
        # before the fix it rejected the bless opcode as "Invalid object type".
        set out [exec src/redis-check-rdb $rdb]
        assert_match "*RDB looks OK*" $out
    } {} {external:skip}

    test {BLESS and TTL coexist across DEBUG RELOAD} {
        r flushall
        r set a 1
        r expire a 10000
        r bless set a no-evict
        r debug reload
        assert_equal {NO-EVICT} [r bless get a]
        assert {[r ttl a] > 0}
    } {} {needs:debug}

    test {BLESS CLEAR removes the flag and it stays cleared across reload} {
        r flushall
        r set a 1
        r bless set a no-evict
        r bless clear a no-evict
        assert_equal 0 [llength [r bless list no-evict]]
        r debug reload
        assert_equal 0 [llength [r bless list no-evict]]
        assert_equal {} [r bless get a]
    } {} {needs:debug}

    test {INFO exposes the instance-wide blessed_keys count} {
        r flushall
        assert_equal 0 [s blessed_keys]
        r set a 1; r set b 2
        r bless set a no-evict
        r bless set b no-evict
        assert_equal 2 [s blessed_keys]
        r bless clear a no-evict
        assert_equal 1 [s blessed_keys]
    }

    test {MEMORY STATS blessed overhead counts the key-name copies} {
        r flushall
        set long [string repeat x 2000]
        r set $long v
        # after flushall + one key, only the current DB is non-empty -> one db.N
        # entry (db.9 in standalone, db.0 in cluster). Look it up rather than hardcode.
        array set st1 [r memory stats]
        set dbkey [lindex [lsort [array names st1 db.*]] 0]
        array set d1 $st1($dbkey)
        set before $d1(overhead.hashtable.blessed)
        r bless set $long no-evict
        array set st2 [r memory stats]
        array set d2 $st2($dbkey)
        set after $d2(overhead.hashtable.blessed)
        # the index now owns a ~2000-byte sdsdup copy of the key name
        assert {$after - $before > 1500}
    }

    test {FLUSH ASYNC wipes the blessed index (freed on BIO, rebuilt empty)} {
        r flushall
        r set a 1; r set b 2
        r bless set a no-evict
        r bless set b no-evict
        assert_equal 2 [s blessed_keys]
        # async path: emptyDbAsync swaps blessed_keys and frees the old one on BIO
        r flushdb async
        assert_equal 0 [s blessed_keys]
        assert_equal 0 [llength [r bless list no-evict]]
        # blessing works again on the fresh index
        r set c 3
        r bless set c no-evict
        assert_equal 1 [s blessed_keys]
        r flushall async
        assert_equal 0 [s blessed_keys]
    }

    test {BLESS SET/CLEAR reuse the metadata slot: no realloc after the first bless} {
        r flushall
        r set k v
        # The one-time 8-byte ATTR slot may hide under allocator rounding, so we
        # don't assert the first bless grows MEMORY USAGE. What must hold on every
        # allocator: across many set/clear cycles the size stays constant - if
        # clear re-grew or leaked the slot each round, 100x would accumulate enough
        # to cross a rounding bucket and show up.
        set blessed 0
        for {set i 0} {$i < 100} {incr i} {
            assert_equal 1 [r bless set k no-evict]
            assert_equal {NO-EVICT} [r bless get k]
            assert_equal 1 [llength [r bless list no-evict]]
            # Capture the size once the slot exists, then require it to stay put.
            if {$i == 0} { set blessed [r memory usage k] }
            # Subsequent SETs reuse the existing slot -> no further growth.
            assert_equal $blessed [r memory usage k]

            assert_equal 1 [r bless clear k no-evict]
            assert_equal {} [r bless get k]
            assert_equal 0 [llength [r bless list no-evict]]
            # CLEAR zeroes the mask but keeps the slot (like PERSIST) -> no shrink.
            assert_equal $blessed [r memory usage k]
        }
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

    test {Blessing a key already in the LRU eviction pool still protects it} {
        r flushall
        r config set maxmemory 0
        r config set maxmemory-policy allkeys-lru
        # Create old keys, then force a first eviction round so the LRU pool fills
        # with (still-unblessed) candidates sampled from these keys.
        for {set j 0} {$j < 300} {incr j} { r set old:$j [string repeat x 1000] }
        set used [s used_memory]
        # tight headroom so the warm phase genuinely overflows and evicts; reset
        # the counter first so the assertion reflects THIS test, not a prior one.
        r config set maxmemory [expr {$used + 100000}]
        r config resetstat
        for {set j 0} {$j < 300} {incr j} { catch {r set warm:$j [string repeat y 1000]} }
        assert {[s evicted_keys] > 0}
        # Lift the limit before blessing: BLESS SET is a DENYOOM write, so while
        # over maxmemory it would trigger eviction and could drop the very old:*
        # key we're about to bless. The survivors are already in the LRU pool from
        # the warm-phase eviction above.
        r config set maxmemory 0
        set blessed {}
        for {set j 0} {$j < 300 && [llength $blessed] < 40} {incr j} {
            if {[r exists old:$j]} {
                r bless set old:$j no-evict
                lappend blessed old:$j
            }
        }
        assert {[llength $blessed] > 0}
        # Heavy pressure again: the pool-based selection runs many times. A stale
        # pooled entry that became blessed must NOT be evicted.
        r config set maxmemory [expr {$used + 100000}]
        for {set j 0} {$j < 4000} {incr j} { catch {r set flood:$j [string repeat z 1000]} }
        foreach k $blessed { assert_equal 1 [r exists $k] }
        r config set maxmemory 0
    }

    test {All-blessed under a pool policy (allkeys-lru): bounded overshoot then OOM} {
        r flushall
        r config set maxmemory 0
        r config set maxmemory-policy allkeys-lru
        for {set j 0} {$j < 500} {incr j} {
            r set b:$j [string repeat y 1000]
            r bless set b:$j no-evict
        }
        set used [s used_memory]
        # Pool-based policy: sampling hits only blessed keys, so no candidate ever
        # enters the pool -> the blessed-only rounds path. Over the limit but within
        # the 1.25x factor -> tolerated, the small write succeeds.
        r config set maxmemory [expr {int($used / 1.15)}]
        assert_equal OK [r set within [string repeat z 100]]
        # Far past the factor -> OOM.
        r config set maxmemory [expr {int($used / 1.4)}]
        assert_error {*OOM*} {r set past [string repeat z 100]}
        r config set maxmemory 0
    }

    test {All-blessed: bounded overshoot tolerated, OOM past the factor, unbless relieves} {
        r flushall
        r config set maxmemory 0
        r config set maxmemory-policy allkeys-random
        for {set j 0} {$j < 500} {incr j} {
            r set b:$j [string repeat y 1000]
            r bless set b:$j no-evict
        }
        assert_equal 500 [llength [r bless list no-evict]]
        set used [s used_memory]

        # Over maxmemory but within the 1.25x factor, and every key is blessed so
        # eviction can free nothing -> tolerate: a small write still succeeds.
        r config set maxmemory [expr {int($used * 0.9)}]
        assert_equal OK [r set small [string repeat z 100]]

        # Far past the 1.25x ceiling -> clean OOM (the hard limit still holds).
        r config set maxmemory [expr {int($used * 0.5)}]
        assert_error {*OOM*} {r set nope [string repeat z 100]}

        # Unbless -> keys become evictable, so eviction works again (no OOM).
        for {set j 0} {$j < 500} {incr j} { r bless clear b:$j no-evict }
        assert_equal 0 [llength [r bless list no-evict]]
        r config set maxmemory [expr {$used - 50000}]
        assert_equal OK [r set afterunbless v]
        r config set maxmemory 0
    }

    test {Blessed overshoot: tolerated just under the 1.25x factor, OOM just over it} {
        r flushall
        r config set maxmemory 0
        r config set maxmemory-policy allkeys-random
        for {set j 0} {$j < 500} {incr j} {
            r set b:$j [string repeat y 1000]
            r bless set b:$j no-evict
        }
        set used [s used_memory]

        # used ~= 1.15x maxmemory: over the limit but within the 1.25x factor.
        # All keys blessed -> eviction frees nothing -> tolerate: the write succeeds.
        r config set maxmemory [expr {int($used / 1.15)}]
        assert {[lindex [r config get maxmemory] 1] < $used}   ;# genuinely over the limit
        assert_equal OK [r set within [string repeat z 100]]

        # used ~= 1.4x maxmemory: past the 1.25x factor -> OOM.
        r config set maxmemory [expr {int($used / 1.4)}]
        assert_error {*OOM*} {r set past [string repeat z 100]}

        r config set maxmemory 0
    }

    test {BLESS SET is DENYOOM, CLEAR is not (unbless to recover under OOM)} {
        r flushall
        r config set maxmemory 0
        r config set maxmemory-policy noeviction
        r set a v
        r set b v
        r bless set b no-evict           ;# bless b while there's headroom
        # force OOM: cap maxmemory below current usage
        set used [s used_memory]
        r config set maxmemory [expr {$used - 100000}]
        # a real DENYOOM write is rejected here...
        assert_error {*OOM*} {r set grow [string repeat z 1000]}
        # ...and BLESS SET is now DENYOOM too -> rejected under OOM.
        assert_error {*OOM*} {r bless set a no-evict}
        assert_equal {} [r bless get a]
        # BLESS CLEAR is NOT DENYOOM -> still works, so you can unbless to recover.
        assert_equal 1 [r bless clear b no-evict]
        assert_equal {} [r bless get b]
        r config set maxmemory 0
    }

    # Under volatile-* only keys WITH a TTL are candidates, so the blessed key and
    # the flood both get TTLs. volatile-lru/ttl use the pool branch, volatile-random
    # the random branch - both sample db->expires, and both must skip the blessed key.
    foreach policy {volatile-lru volatile-ttl volatile-random} {
        test "BLESS NO-EVICT protects a key with a TTL under $policy" {
            r flushall
            r config set maxmemory 0
            r config set maxmemory-policy $policy
            r set precious [string repeat x 2000]
            r expire precious 100000
            r bless set precious no-evict
            set used [s used_memory]
            r config set maxmemory [expr {$used + 3000000}]
            for {set j 0} {$j < 4000} {incr j} {
                r set flood:$j [string repeat y 2000] ex 100000
            }
            assert {[s evicted_keys] > 0}
            assert_equal 1 [r exists precious]
            assert_equal {NO-EVICT} [r bless get precious]
            r config set maxmemory 0
        }
    }

    # LFU shares the pool branch with LRU but computes idle differently; make sure
    # the blessed-key skip holds on that path too.
    test {BLESS NO-EVICT key is not evicted under allkeys-lfu} {
        r flushall
        r config set maxmemory 0
        r config set maxmemory-policy allkeys-lfu
        r set precious [string repeat x 2000]
        r bless set precious no-evict
        set used [s used_memory]
        r config set maxmemory [expr {$used + 3000000}]
        for {set j 0} {$j < 4000} {incr j} { r set flood:$j [string repeat y 2000] }
        assert {[s evicted_keys] > 0}
        assert_equal 1 [r exists precious]
        assert_equal {NO-EVICT} [r bless get precious]
        r config set maxmemory 0
    }

    # A large blessed majority with a small unblessed minority. The blessed-only
    # sampling retry must not permanently give up on the (few) real victims: under
    # sustained pressure the unblessed keys are reclaimed and writes keep succeeding
    # (no spurious lasting OOM), while the blessed majority stays intact.
    test {Unblessed victims are still evicted when most of the keyspace is blessed} {
        r flushall
        r config set maxmemory 0
        r config set maxmemory-policy allkeys-random
        for {set j 0} {$j < 500} {incr j} {
            r set b:$j [string repeat y 1000]
            r bless set b:$j no-evict
        }
        for {set j 0} {$j < 50} {incr j} { r set victim:$j [string repeat z 1000] }
        assert_equal 500 [llength [r bless list no-evict]]
        set used [s used_memory]
        # tight headroom -> every write forces eviction of the unblessed minority
        r config set maxmemory [expr {$used + 100000}]
        for {set j 0} {$j < 3000} {incr j} { assert_equal OK [r set flood:$j [string repeat w 1000]] }
        # blessed majority untouched ...
        assert_equal 500 [llength [r bless list no-evict]]
        # ... and the unblessed victims were reclaimed, not falsely protected
        set survivors 0
        for {set j 0} {$j < 50} {incr j} { incr survivors [r exists victim:$j] }
        assert {$survivors < 50}
        r config set maxmemory 0
    }
}
