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

    test {hash snapshot: writes to a DB with no open snapshot are not captured} {
        r flushall
        r hset sc f1 a
        set s [r debug kvsnapshot create]
        set d0 [dict get [r debug kvsnapshot stats] hash_deltas]
        set b0 [dict get [r debug kvsnapshot stats] preserved_bytes]
        r select 1
        r hset other f1 a f2 b
        r hset other f1 X
        r hdel other f2
        r del other
        assert_equal $d0 [dict get [r debug kvsnapshot stats] hash_deltas]
        assert_equal $b0 [dict get [r debug kvsnapshot stats] preserved_bytes]
        r flushdb
        r select 9
        r hset sc f1 X          ;# the in-scope DB still captures
        assert {[dict get [r debug kvsnapshot stats] hash_deltas] > $d0}
        assert_equal a [r debug kvsnapshot hget $s sc f1]
        r debug kvsnapshot free $s
    } {OK} {singledb:skip}

    test {hash snapshot: preserved-bytes accounting grows and returns to zero} {
        r flushall
        assert_equal 0 [dict get [r debug kvsnapshot stats] preserved_bytes]
        set v [string repeat a 300]
        r hset ab f1 $v f2 $v
        set s1 [r debug kvsnapshot create]
        r hset ab f1 [string repeat b 300]   ;# one pre-image chain record
        set b1 [dict get [r debug kvsnapshot stats] preserved_bytes]
        assert {$b1 > 300}
        set s2 [r debug kvsnapshot create]
        r del ab                             ;# freezes a whole hash into both
        set b2 [dict get [r debug kvsnapshot stats] preserved_bytes]
        assert {$b2 > $b1}
        assert_equal $b2 [s hash_snapshot_preserved_bytes]
        r debug kvsnapshot free $s1
        set b3 [dict get [r debug kvsnapshot stats] preserved_bytes]
        assert {$b3 > 0 && $b3 < $b2}
        r debug kvsnapshot free $s2
        assert_equal 0 [dict get [r debug kvsnapshot stats] preserved_bytes]
        assert_equal 0 [s hash_snapshot_preserved_bytes]
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

    test {hash snapshot: evicted key is dropped entirely, never partially} {
        r flushall
        set big [string repeat x 4096]
        r hset ev f1 $big f2 $big f3 $big
        r expire ev 10000            ;# the only volatile key => the only evict candidate
        set s [r debug kvsnapshot create]
        r hset ev f1 [string repeat y 4096]   ;# delta chain for f1 ONLY
        # Per-field reads only, so the key is never materialized; the already-frozen
        # case is covered by the next test.
        assert_equal $big [r debug kvsnapshot hget $s ev f1]
        assert_equal $big [r debug kvsnapshot hget $s ev f2]
        set drops0 [dict get [r debug kvsnapshot stats] evicted_drops]

        # Eviction driver as in moduleapi/hooks.tcl.
        set used [expr {[s used_memory] - [s mem_not_counted_for_evict]}]
        set old_policy [lindex [r config get maxmemory-policy] 1]
        r config set maxmemory [expr {$used+100*1024}]
        r config set maxmemory-policy volatile-random
        r setbit big-key 1600000 0   ;# ~200kb, pushes past the cap
        r getbit big-key 0           ;# this command triggers the eviction
        r config set maxmemory-policy $old_policy
        r config set maxmemory 0

        assert_equal 0 [r exists ev]
        assert_equal 1 [expr {[dict get [r debug kvsnapshot stats] evicted_drops] - $drops0}]
        assert_equal {} [r debug kvsnapshot hget $s ev f1] ;# must NOT resurrect
        assert_equal {} [r debug kvsnapshot hget $s ev f2]
        assert_equal {} [r debug kvsnapshot hget $s ev f3]
        assert_equal -1 [r debug kvsnapshot len $s ev]     ;# absent, not a short hash
        r debug kvsnapshot free $s
        r del big-key
    } {1}

    test {hash snapshot: eviction drops an already-materialized frozen copy} {
        r flushall
        set big [string repeat x 4096]
        r hset mv f1 $big f2 $big f3 $big
        r expire mv 10000
        set s [r debug kvsnapshot create]
        r hset mv f1 [string repeat y 4096]   ;# chain for f1
        set b0 [dict get [r debug kvsnapshot stats] preserved_bytes]
        assert_equal 3 [r debug kvsnapshot len $s mv] ;# materializes into `preserved`
        set b1 [dict get [r debug kvsnapshot stats] preserved_bytes]
        assert {$b1 > $b0}
        set drops0 [dict get [r debug kvsnapshot stats] evicted_drops]

        set used [expr {[s used_memory] - [s mem_not_counted_for_evict]}]
        set old_policy [lindex [r config get maxmemory-policy] 1]
        r config set maxmemory [expr {$used+100*1024}]
        r config set maxmemory-policy volatile-random
        r setbit big-key 1600000 0
        r getbit big-key 0
        r config set maxmemory-policy $old_policy
        r config set maxmemory 0

        assert_equal 0 [r exists mv]
        assert_equal 1 [expr {[dict get [r debug kvsnapshot stats] evicted_drops] - $drops0}]
        assert_equal -1 [r debug kvsnapshot len $s mv]     ;# frozen copy dropped too
        assert_equal {} [r debug kvsnapshot hget $s mv f1] ;# must NOT resurrect
        assert_equal {} [r debug kvsnapshot hget $s mv f2]
        # The frozen copy AND the chain are both released, so this lands below $b0.
        assert_equal 0 [dict get [r debug kvsnapshot stats] preserved_bytes]
        r debug kvsnapshot free $s
        r del big-key
    } {1}

    test {hash snapshot: SWAPDB retargets open snapshots to follow the keyspace} {
        r flushall
        r select 1
        r hset sw f1 other1 f2 other2 f3 other3   ;# same key name, other keyspace
        r select 9
        r hset sw f1 a f2 b f3 c
        set s [r debug kvsnapshot create]
        r hset sw f1 X               ;# chain for f1 only
        set d0 [dict get [r debug kvsnapshot stats] hash_deltas]

        r swapdb 9 1                 ;# the snapshotted keyspace is now at index 1

        assert_equal a [r debug kvsnapshot hget $s sw f1] ;# chain still resolves
        assert_equal b [r debug kvsnapshot hget $s sw f2] ;# live fallback followed it
        r select 1
        r hset sw f3 Z               ;# writes there are still captured
        assert {[dict get [r debug kvsnapshot stats] hash_deltas] > $d0}
        assert_equal c [r debug kvsnapshot hget $s sw f3]
        r del sw                     ;# and a DEL there still materializes
        assert_equal b [r debug kvsnapshot hget $s sw f2]
        assert_equal 3 [r debug kvsnapshot len $s sw]
        r select 9
        assert_equal other1 [r hget sw f1] ;# the swapped-in keyspace never bled through
        r debug kvsnapshot free $s
        r flushall
    } {OK} {singledb:skip}

    test {hash snapshot: an EXPIRED key is still preserved (only eviction is exempt)} {
        r flushall
        r hset xp f1 a f2 b
        r pexpire xp 40
        set s [r debug kvsnapshot create]
        set drops0 [dict get [r debug kvsnapshot stats] evicted_drops]
        wait_for_condition 50 20 {
            [r dbsize] == 0
        } else {
            fail "key xp was not expired"
        }
        assert_equal a [r debug kvsnapshot hget $s xp f1]
        assert_equal 2 [r debug kvsnapshot len $s xp]
        assert_equal $drops0 [dict get [r debug kvsnapshot stats] evicted_drops]
        r debug kvsnapshot free $s
    } {OK}

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

# A diskless full-sync swap discards the snapshotted keyspace, so open snapshots
# are invalidated rather than retargeted. Driven through DEBUG KVSNAPSHOT with no
# module loaded: repl-diskless-load swapdb is disabled unless every loaded module
# declares REDISMODULE_OPTIONS_HANDLE_REPL_ASYNC_LOAD.
start_server {tags {"modules external:skip"}} {
    set replica [srv 0 client]
    set replica_host [srv 0 host]
    set replica_port [srv 0 port]
    start_server {} {
        set master [srv 0 client]
        set master_host [srv 0 host]
        set master_port [srv 0 port]

        test {hash snapshot: diskless full-sync swap invalidates open snapshots} {
            $master config set repl-diskless-sync yes
            $master config set repl-diskless-sync-delay 0
            $replica config set repl-diskless-load swapdb
            $master hset onmaster f1 m1

            $replica hset local f1 a f2 b
            set s [$replica debug kvsnapshot create]
            $replica hset local f1 X    ;# chain for f1
            assert_equal a [$replica debug kvsnapshot hget $s local f1]
            assert_equal 2 [$replica debug kvsnapshot len $s local] ;# also freeze a copy
            assert {[dict get [$replica debug kvsnapshot stats] preserved_bytes] > 0}
            assert_equal 1 [dict get [$replica debug kvsnapshot stats] snapshots_open]

            set loglines [count_log_lines -1]
            $replica replicaof $master_host $master_port
            wait_for_sync $replica
            # Assert the swap path actually ran, so this can't pass vacuously.
            wait_for_log_messages -1 {"*Swapping active DB with loaded DB*"} $loglines 50 100

            assert_equal 0 [$replica exists local]
            assert_equal m1 [$replica hget onmaster f1]
            # Invalidated: reads are absent, held memory released, write gate at zero.
            assert_equal {} [$replica debug kvsnapshot hget $s local f1]
            assert_equal -1 [$replica debug kvsnapshot len $s local]
            assert_equal 0 [dict get [$replica debug kvsnapshot stats] preserved_bytes]
            assert_equal 0 [dict get [$replica debug kvsnapshot stats] snapshots_open]

            # A second swap must not decrement the already-invalid snapshot again.
            set loglines [count_log_lines -1]
            $replica replicaof no one
            $replica replicaof $master_host $master_port
            wait_for_sync $replica
            wait_for_log_messages -1 {"*Swapping active DB with loaded DB*"} $loglines 50 100
            assert_equal 0 [dict get [$replica debug kvsnapshot stats] snapshots_open]
            # Nor may freeing an already-invalidated snapshot.
            $replica debug kvsnapshot free $s
            assert_equal 0 [dict get [$replica debug kvsnapshot stats] snapshots_open]
            $replica replicaof no one
        } {OK}
    }
}
