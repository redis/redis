set testmodule [file normalize tests/modules/kvsnapshot.so]

start_server {tags {"modules external:skip"}} {
    r module load $testmodule

    # In-module scenario: field reconstruction (changed/deleted/unchanged/added),
    # cheap per-field read, whole-key delete + overwrite materialize, and that the
    # snapshot API rejects non-hash keys.
    test {hash snapshot: values frozen across mutation, delete, overwrite} {
        assert_equal OK [r kvsnap.test]
    }

    test {hash snapshot: field reconstruction incl. HDEL + materialize} {
        r flushall
        r hset dh f1 a f2 b f3 c
        set s [r debug kvsnapshot create]
        r hset dh f1 X   ;# change
        r hdel dh f2     ;# delete
        r hset dh f4 d   ;# add
        assert_equal a  [r debug kvsnapshot hget $s dh f1] ;# changed -> old value
        assert_equal b  [r debug kvsnapshot hget $s dh f2] ;# deleted -> recovered old
        assert_equal c  [r debug kvsnapshot hget $s dh f3] ;# unchanged
        assert_equal {} [r debug kvsnapshot hget $s dh f4] ;# added -> absent as-of-V
        assert_equal 3  [r debug kvsnapshot len $s dh]     ;# as-of-V = {f1,f2,f3}
        assert_equal X  [r hget dh f1]                     ;# live reflects the change
        r debug kvsnapshot free $s
    }

    test {hash snapshot: whole-key DELETE materializes as-of-V} {
        r flushall
        r hset dd x 1 y 2
        set s [r debug kvsnapshot create]
        r hset dd x 9 ;# change x -> recorded
        r del dd
        assert_equal 1 [r debug kvsnapshot hget $s dd x] ;# changed field, as-of-V
        assert_equal 2 [r debug kvsnapshot hget $s dd y] ;# untouched, from live-at-delete
        assert_equal 2 [r debug kvsnapshot len $s dd]
        assert_equal 0 [r exists dd]                     ;# live gone
        r debug kvsnapshot free $s
    }

    test {hash snapshot: whole-key OVERWRITE materializes as-of-V before replace} {
        r flushall
        r hset ov a 1 b 2
        set s [r debug kvsnapshot create]
        r hset ov a 99
        r set ov nowastring          ;# overwrite the hash with a string
        assert_equal 1 [r debug kvsnapshot hget $s ov a]
        assert_equal 2 [r debug kvsnapshot hget $s ov b]
        assert_equal 2 [r debug kvsnapshot len $s ov]
        assert_equal string [r type ov]
        r debug kvsnapshot free $s
    }

    test {hash snapshot: hash-field TTL (HFE) expiry recovered as-of-V} {
        r flushall
        r hset hx f1 a f2 b
        set s [r debug kvsnapshot create]
        r hpexpire hx 20 fields 1 f2  ;# f2 expires soon
        after 60
        r hget hx f2                  ;# trigger lazy expiry if not active-expired
        assert_equal b [r debug kvsnapshot hget $s hx f2] ;# expired field recovered as-of-V
        assert_equal a [r debug kvsnapshot hget $s hx f1] ;# unchanged
        assert_equal 2 [r debug kvsnapshot len $s hx]     ;# as-of-V = {f1,f2}
        r debug kvsnapshot free $s
    }

    test {hash snapshot: materialized hash keeps HFE fields past wall-clock expiry} {
        # A field alive as-of-V whose TTL lapses (in wall-clock) AFTER the snapshot
        # must still read from the materialized (preserved) hash -- it was valid at V.
        r flushall
        r hset ph f1 a f2 b
        r hpexpire ph 40 fields 1 f2     ;# f2 expires 40ms from now (after V)
        set s [r debug kvsnapshot create]
        r del ph                          ;# whole-key delete -> materialize now (f2 still alive)
        after 90                          ;# wall-clock passes f2's copied expiry
        assert_equal a [r debug kvsnapshot hget $s ph f1]
        assert_equal b [r debug kvsnapshot hget $s ph f2] ;# still readable as-of-V
        r debug kvsnapshot free $s
    }

    test {hash snapshot: unchanged field with post-V TTL still reads as-of-V} {
        # A field never written after V (so no delta) whose TTL lapses in wall-clock
        # after the snapshot must still read as-of-V via the live-fallback path --
        # consistent with the materialized and captured paths.
        r flushall
        r hset uh f1 a f2 b
        r hpexpire uh 40 fields 1 f2      ;# f2 TTL 40ms from now (after V)
        set s [r debug kvsnapshot create]
        after 90                          ;# f2's TTL lapses; f2 unchanged since V
        assert_equal b [r debug kvsnapshot hget $s uh f2] ;# as-of-V, still readable
        assert_equal a [r debug kvsnapshot hget $s uh f1]
        r debug kvsnapshot free $s
    }

    test {hash snapshot: key with post-V key-level TTL still reads as-of-V} {
        # A whole key alive as-of-V whose key-level TTL lapses in wall-clock after
        # the snapshot must still read as-of-V, whether it was reaped (materialized
        # on delete) or lingers unreaped (live-fallback with ACCESS_EXPIRED).
        r flushall
        r hset kt f1 a f2 b
        r pexpire kt 40                   ;# whole-key TTL 40ms from now (after V)
        set s [r debug kvsnapshot create]
        after 90                          ;# key TTL lapses; key unchanged since V
        assert_equal a [r debug kvsnapshot hget $s kt f1]
        assert_equal b [r debug kvsnapshot hget $s kt f2]
        assert_equal 2 [r debug kvsnapshot len $s kt]
        r debug kvsnapshot free $s
    }

    test {hash snapshot: write cost is O(1) in open-snapshot count (shared store)} {
        r flushall
        r hset mh f1 a f2 b f3 c
        # Open several snapshots at distinct epochs, then change fields once.
        set snaps {}
        for {set i 0} {$i < 5} {incr i} { lappend snaps [r debug kvsnapshot create] }
        set d0 [dict get [r debug kvsnapshot stats] hash_deltas]
        r hset mh f1 X ;# one changed field -> ONE shared record, not one per snapshot
        r hset mh f2 Y
        set recorded [expr {[dict get [r debug kvsnapshot stats] hash_deltas] - $d0}]
        assert_equal 2 $recorded          ;# 2 fields changed => 2 records, independent of #snapshots
        # every snapshot (all older than the writes) sees the as-of-V values
        foreach sn $snaps {
            assert_equal a [r debug kvsnapshot hget $sn mh f1]
            assert_equal b [r debug kvsnapshot hget $sn mh f2]
        }
        foreach sn $snaps { r debug kvsnapshot free $sn }
    }

    test {hash snapshot: FLUSHALL preserves as-of-V and clears stale store} {
        r flushall
        r hset fk a 1 b 2
        set s [r debug kvsnapshot create]
        r hset fk a 9        ;# change a -> recorded in the shared store
        r flushall           ;# wipes the keyspace, must materialize into snapshot
        assert_equal 1 [r debug kvsnapshot hget $s fk a] ;# changed field, as-of-V
        assert_equal 2 [r debug kvsnapshot hget $s fk b] ;# untouched, preserved
        assert_equal 2 [r debug kvsnapshot len $s fk]    ;# whole hash preserved
        assert_equal 0 [r exists fk]                     ;# live gone
        # A key recreated with the same name must NOT inherit the stale chain,
        # and the snapshot must keep its as-of-V view.
        r hset fk a 100
        assert_equal 1   [r debug kvsnapshot hget $s fk a]
        assert_equal 100 [r hget fk a]
        r debug kvsnapshot free $s
    }

    test {snapshot read from a background thread (no concurrent write)} {
        r del doc
        r hset doc f original
        set rd [redis_deferring_client]
        $rd kvsnap.threadget doc f 50
        assert_equal original [$rd read]
        $rd close
    }

    test {snapshot read from a background thread is consistent across a concurrent write} {
        r del doc
        r hset doc f original
        set rd [redis_deferring_client]
        $rd kvsnap.threadget doc f 300
        after 80              ;# let the command create the snapshot and block
        r hset doc f changed  ;# concurrent write during the worker's sleep
        assert_equal original [$rd read]   ;# the worker saw the pre-write value
        assert_equal changed [r hget doc f];# live reflects the write
        $rd close
    }
}
