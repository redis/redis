start_server {tags {"bless"}} {
    test {BLESS SET/GET/COUNT basics} {
        r flushall
        r set k v
        # SET NO-EVICT protects; reply 1 on change, 0 on no-op
        assert_equal 1 [r bless set k no-evict]
        assert_equal 0 [r bless set k no-evict]
        assert_equal {NO-EVICT} [r bless get k]
        assert_equal 1 [r bless count]
        # a second key
        r set k2 v
        assert_equal 1 [r bless set k2 no-evict]
        assert_equal {NO-EVICT} [r bless get k2]
        assert_equal 2 [r bless count]
        # CLEAR removes the protection
        assert_equal 1 [r bless clear k no-evict]
        assert_equal {} [r bless get k]
        assert_equal 1 [r bless count]
        # COUNT takes an optional flag like LIST (default NO-EVICT)
        assert_equal 1 [r bless count no-evict]
        assert_error {*syntax*} {r bless count none}
        assert_error {*syntax*} {r bless count inram}
        assert_error {*syntax*} {r bless count bogus}
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
        assert_error {*syntax*} {r bless set k inram}
        assert_equal 1 [r bless set k no-evict]
        assert_equal {NO-EVICT} [r bless get k]
    }

    test {BLESS LIST returns keys with the given flag (default NO-EVICT)} {
        r flushall
        r set a 1; r set b 2; r set c 3
        r bless set a no-evict
        r bless set b no-evict
        assert_equal [lsort {a b}] [lsort [r bless list]]
        assert_equal [lsort {a b}] [lsort [r bless list no-evict]]
        # CLEAR removes the key from the list
        r bless clear a no-evict
        assert_equal {b} [r bless list]
        # LIST accepts only NO-EVICT (or nothing); INRAM future
        assert_error {*syntax*} {r bless list bogus}
        assert_error {*syntax*} {r bless list none}
        assert_error {*syntax*} {r bless list inram}
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
        # CLEAR and key removal are the only things that clear it
        r bless clear k no-evict
        assert_equal {} [r bless get k]
        r del h
        r set h x
        assert_equal {} [r bless get h]
        assert_equal 1 [r bless count]
    }

    test {SET keeps blessing; DEL+SET clears it} {
        r flushall
        # SET, BLESS, SET  -> blessing kept across the overwrite
        r set k v1
        r bless set k no-evict
        r set k v2
        assert_equal {NO-EVICT} [r bless get k]
        assert_equal 1 [r bless count]
        # SET, DEL, SET  -> key removal clears the blessing; the recreated key is plain
        r del k
        r set k v3
        assert_equal {} [r bless get k]
        assert_equal 0 [r bless count]
    }

    test {MOVE transfers the blessing and leaves no ghost in the source index} {
        r flushall
        r set k v
        r bless set k no-evict
        assert_equal 1 [r bless count]
        r move k 10
        # source DB (9): key gone, index has no ghost entry
        assert_equal 0 [r exists k]
        assert_equal 0 [r bless count]
        assert_equal {} [r bless list]
        # destination DB (10): key present and still blessed
        r select 10
        assert_equal {NO-EVICT} [r bless get k]
        assert_equal 1 [r bless count]
        r flushall
        r select 9
    } {OK} {cluster:skip}

    test {BLESS survives DEBUG RELOAD (RDB round-trip)} {
        r flushall
        r set a 1; r set b 2; r set c 3
        r bless set a no-evict
        r bless set b no-evict
        assert_equal 2 [r bless count]
        r debug reload
        # index rebuilt on load; flags and values intact
        assert_equal 2 [r bless count]
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
        assert_equal 0 [r bless count]
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
        assert_equal 0 [r bless count]
        r debug reload
        assert_equal 0 [r bless count]
        assert_equal {} [r bless get a]
    } {} {needs:debug}

    test {bless-max-keys: config get/set, default 1024} {
        r flushall
        assert_equal {bless-max-keys 1024} [r config get bless-max-keys]
        r config set bless-max-keys 3
        assert_equal {bless-max-keys 3} [r config get bless-max-keys]
    }

    test {bless-max-keys: cap blocks new blessings, unbless frees a slot} {
        r flushall
        r config set bless-max-keys 3
        for {set j 0} {$j < 5} {incr j} { r set k:$j v }
        # up to the cap succeeds
        for {set j 0} {$j < 3} {incr j} { assert_equal 1 [r bless set k:$j no-evict] }
        assert_equal 3 [r bless count]
        # the next new blessing is refused
        assert_error {*bless-max-keys*} {r bless set k:3 no-evict}
        assert_equal {} [r bless get k:3]
        # re-blessing an already-blessed key is exempt (count doesn't grow)
        assert_equal 0 [r bless set k:0 no-evict]
        # unbless one -> a slot frees up
        r bless clear k:0 no-evict
        assert_equal 1 [r bless set k:3 no-evict]
        assert_equal 3 [r bless count]
    }

    test {bless-max-keys: 0 means unlimited} {
        r flushall
        r config set bless-max-keys 0
        for {set j 0} {$j < 50} {incr j} { r set u:$j v; r bless set u:$j no-evict }
        assert_equal 50 [r bless count]
        r config set bless-max-keys 1024
    }

    test {bless-max-keys: lowering below current count keeps existing, blocks new} {
        r flushall
        r config set bless-max-keys 1024
        for {set j 0} {$j < 5} {incr j} { r set k:$j v; r bless set k:$j no-evict }
        assert_equal 5 [r bless count]
        # lower the cap under the current count
        r config set bless-max-keys 2
        assert_equal 5 [r bless count]           ;# existing blessings untouched
        r set k:new v
        assert_error {*bless-max-keys*} {r bless set k:new no-evict}
        r config set bless-max-keys 1024
    }

    test {bless-max-keys: load bypasses the cap (DEBUG RELOAD)} {
        r flushall
        r config set bless-max-keys 1024
        for {set j 0} {$j < 5} {incr j} { r set k:$j v; r bless set k:$j no-evict }
        r config set bless-max-keys 2
        r debug reload
        r config set bless-max-keys 1024
        # all 5 survive the load even though the cap was 2 during the reload
        assert_equal 5 [r bless count]
    } {} {needs:debug}

    test {bless-max-keys: AOF replay bypasses the cap (mustObeyClient)} {
        r flushall
        r config set bless-max-keys 1024
        r config set appendonly yes
        waitForBgrewriteaof r
        # blessings recorded in the incr AOF as BLESS SET commands
        for {set j 0} {$j < 5} {incr j} { r set k:$j v; r bless set k:$j no-evict }
        assert_equal 5 [r bless count]
        # lower the cap, then replay the AOF: the BLESS SET commands come from
        # the AOF client (mustObeyClient), so they must not be rejected.
        r config set bless-max-keys 2
        r debug loadaof
        r config set appendonly no
        r config set bless-max-keys 1024
        assert_equal 5 [r bless count]
    } {} {needs:debug}
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
        r config set maxmemory [expr {$used + 500000}]
        for {set j 0} {$j < 300} {incr j} { catch {r set warm:$j [string repeat y 1000]} }
        assert {[s evicted_keys] > 0}
        # Bless survivors AFTER they may already sit in the pool. Cap the count so
        # blessed keys never exhaust maxmemory on their own.
        set blessed {}
        for {set j 0} {$j < 300 && [llength $blessed] < 40} {incr j} {
            if {[r exists old:$j]} {
                r bless set old:$j no-evict
                lappend blessed old:$j
            }
        }
        assert {[llength $blessed] > 0}
        # Heavy pressure: the pool-based selection runs many times. A stale pooled
        # entry that became blessed must NOT be evicted.
        for {set j 0} {$j < 4000} {incr j} { catch {r set flood:$j [string repeat z 1000]} }
        foreach k $blessed { assert_equal 1 [r exists $k] }
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
        assert_equal 500 [r bless count]
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
        assert_equal 0 [r bless count]
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
}
