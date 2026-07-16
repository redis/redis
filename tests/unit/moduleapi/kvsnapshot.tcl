set testmodule [file normalize tests/modules/kvsnapshot.so]

start_server {tags {"modules external:skip"}} {
    r module load $testmodule

    test {keyspace value-MVCC snapshot: values frozen across mutation and delete} {
        assert_equal OK [r kvsnap.test]
    }

    test {keyspace snapshot version advances with writes} {
        set v1 [dict get [r debug kvsnapshot stats] keyspace_version]
        r set somerandomkey someval
        set v2 [dict get [r debug kvsnapshot stats] keyspace_version]
        assert {$v2 > $v1}
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
        # NOTE: r and the deferring client must be on the same DB — snapshots are
        # DB-scoped. Both default to the test DB here.
        r del doc
        r hset doc f original
        set rd [redis_deferring_client]
        # Blocks: the snapshot is created now (main thread), the worker sleeps
        # 300ms and then reads doc.f as-of the snapshot under the GIL.
        $rd kvsnap.threadget doc f 300
        after 80             ;# let the command create the snapshot and block
        r hset doc f changed ;# concurrent write on the main thread during the worker's sleep
        assert_equal original [$rd read]   ;# the worker saw the pre-write value
        assert_equal changed [r hget doc f];# live reflects the write
        $rd close
    }

    test {hash delta-log: field reconstruction incl. HDEL + materialize, no deep copy} {
        r flushall
        r hset dh f1 a f2 b
        set s [r debug kvsnapshot create]
        r debug kvsnapshot deltahash $s
        set c0 [dict get [r debug kvsnapshot stats] cow_copies]
        r hset dh f1 X ;# change an existing field
        r hset dh f3 Z ;# add a new field
        r hdel dh f2   ;# delete an existing field
        assert_equal a  [r debug kvsnapshot hget $s dh f1] ;# changed -> old value
        assert_equal b  [r debug kvsnapshot hget $s dh f2] ;# deleted -> recovered old value
        assert_equal {} [r debug kvsnapshot hget $s dh f3] ;# new -> absent as-of-V
        assert_equal 2  [r debug kvsnapshot len $s dh]     ;# materialize: as-of-V = {f1,f2}
        assert_equal X  [r hget dh f1]                     ;# live reflects the change
        assert_equal 0  [r hexists dh f2]                  ;# live: f2 deleted
        assert_equal 0  [expr {[dict get [r debug kvsnapshot stats] cow_copies] - $c0}]
        r debug kvsnapshot free $s
    }

    test {hash delta-log: whole-key DELETE materializes as-of-V} {
        r flushall
        r hset dd x 1 y 2
        set s [r debug kvsnapshot create]
        r debug kvsnapshot deltahash $s
        r hset dd x 99 ;# change x -> delta
        r del dd       ;# whole-key delete -> materialize before free
        assert_equal 1 [r debug kvsnapshot hget $s dd x] ;# changed field, as-of-V
        assert_equal 2 [r debug kvsnapshot hget $s dd y] ;# untouched, from live-at-delete
        assert_equal 2 [r debug kvsnapshot len $s dd]
        assert_equal 0 [r exists dd]                     ;# live gone
        r debug kvsnapshot free $s
    }

    test {hash delta-log: whole-key OVERWRITE materializes as-of-V before replace} {
        r flushall
        r hset ov a 1 b 2
        set s [r debug kvsnapshot create]
        r debug kvsnapshot deltahash $s
        r hset ov a 99                ;# change a -> delta recorded
        r set ov newstring            ;# overwrite the hash with a string (dbSetValue)
        assert_equal 1 [r debug kvsnapshot hget $s ov a] ;# changed field, as-of-V
        assert_equal 2 [r debug kvsnapshot hget $s ov b] ;# untouched, from live-at-overwrite
        assert_equal 2 [r debug kvsnapshot len $s ov]    ;# as-of-V hash reconstructed
        assert_equal newstring [r get ov]                ;# live is the new string
        assert_equal string [r type ov]
        r debug kvsnapshot free $s
    }

    test {hash delta-log: hash-field TTL (HFE) expiry recovered as-of-V} {
        r flushall
        r hset hx f1 a f2 b
        set s [r debug kvsnapshot create]
        r debug kvsnapshot deltahash $s
        r hpexpire hx 10 FIELDS 1 f2 ;# f2 expires in 10ms
        after 50                     ;# let it lapse; active or lazy expiry will remove it
        r hget hx f2                 ;# trigger lazy expiry if not already active-expired
        assert_equal 0 [r hexists hx f2]                 ;# live: f2 gone
        assert_equal b [r debug kvsnapshot hget $s hx f2] ;# expired field recovered as-of-V
        assert_equal a [r debug kvsnapshot hget $s hx f1] ;# unchanged field
        assert_equal 2 [r debug kvsnapshot len $s hx]     ;# as-of-V = {f1,f2}
        r debug kvsnapshot free $s
    }

    test {hash delta-log: HFE expiry recovered as-of-V (HT-encoded)} {
        r flushall
        set orig [lindex [r config get hash-max-listpack-entries] 1]
        r config set hash-max-listpack-entries 0 ;# force OBJ_ENCODING_HT
        r hset hh f1 a f2 b
        assert_equal hashtable [r object encoding hh]
        set s [r debug kvsnapshot create]
        r debug kvsnapshot deltahash $s
        r hpexpire hh 10 FIELDS 1 f2
        after 50
        r hget hh f2                 ;# lazy expiry if active hasn't fired
        assert_equal 0 [r hexists hh f2]
        assert_equal b [r debug kvsnapshot hget $s hh f2] ;# expired field recovered
        assert_equal a [r debug kvsnapshot hget $s hh f1]
        assert_equal 2 [r debug kvsnapshot len $s hh]
        r debug kvsnapshot free $s
        r config set hash-max-listpack-entries $orig
    }

    test {hash delta-log: delta-map cap collapses to frozen under churn} {
        r flushall
        r hset dc a 1 b 2 c 3
        set s [r debug kvsnapshot create]
        r debug kvsnapshot deltahash $s
        r debug kvsnapshot deltacap $s 3 ;# collapse once 3 field-deltas accumulate
        set d0 [dict get [r debug kvsnapshot stats] hash_deltas]
        r hset dc a 11 ;# delta a=1
        r hset dc b 22 ;# delta b=2
        r hset dc c 33 ;# delta c=3  (map now at cap)
        for {set i 0} {$i < 20} {incr i} { r hset dc n$i x } ;# next distinct field collapses
        assert_equal 1  [r debug kvsnapshot hget $s dc a]  ;# as-of-V preserved via frozen copy
        assert_equal 2  [r debug kvsnapshot hget $s dc b]
        assert_equal 3  [r debug kvsnapshot hget $s dc c]
        assert_equal {} [r debug kvsnapshot hget $s dc n0] ;# new field absent as-of-V
        assert_equal 3  [r debug kvsnapshot len $s dc]     ;# as-of-V = {a,b,c}
        # Collapse bounded the delta count: only ~3 recorded despite 23 field writes.
        assert {[expr {[dict get [r debug kvsnapshot stats] hash_deltas] - $d0}] <= 4}
        assert_equal 23 [r hlen dc]                        ;# live: a,b,c + n0..n19
        r debug kvsnapshot free $s
    }
}
