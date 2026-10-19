# BLESS NO-EVICT across replication: the command stream, every full sync path,
# replica write rejection, and enforcement after a replica is promoted.

# Every key carrying NO-EVICT in the current DB of $r, from a full BLESS SCAN.
proc bless_scan_all {r} {
    set cursor 0
    set keys {}
    while 1 {
        set res [$r bless scan $cursor no-evict]
        set cursor [lindex $res 0]
        lappend keys {*}[lindex $res 1]
        if {$cursor == 0} break
    }
    lsort -unique $keys
}

start_server {tags {"repl" "bless" "external:skip"}} {
    set replica [srv 0 client]
    start_server {} {
        set master [srv 0 client]
        $replica replicaof [srv 0 host] [srv 0 port]
        wait_for_sync $replica

        test {BLESS SET/CLEAR propagate to the replica over the command stream} {
            $master set a 1
            $master set b 2
            $master rpush l x y
            assert_equal 1 [$master bless set a no-evict]
            assert_equal 1 [$master bless set l no-evict]
            wait_for_ofs_sync $master $replica
            assert_equal {NO-EVICT} [$replica bless get a]
            assert_equal {} [$replica bless get b]
            assert_equal {NO-EVICT} [$replica bless get l]
            assert_equal {a l} [bless_scan_all $replica]
            assert_equal 2 [status $replica blessed_keys]

            assert_equal 1 [$master bless clear a no-evict]
            wait_for_ofs_sync $master $replica
            assert_equal {} [$replica bless get a]
            assert_equal {l} [bless_scan_all $replica]
            assert_equal 1 [status $replica blessed_keys]
        }

        test {A replica rejects BLESS SET and BLESS CLEAR} {
            assert_error {*READONLY*} {$replica bless set b no-evict}
            assert_error {*READONLY*} {$replica bless clear l no-evict}
            assert_equal {} [$replica bless get b]
            assert_equal {NO-EVICT} [$replica bless get l]
        }
    }
}

# The replica starts with a blessed key of its own, which the full sync must
# discard from the index. With repl-diskless-load swapdb the dataset is loaded
# into a temporary DB and swapped in, which also swaps the blessed index.
foreach {sync load} {disk disabled diskless disabled diskless swapdb diskless on-empty-db} {
    start_server {tags {"repl" "bless" "external:skip"}} {
        set replica [srv 0 client]
        start_server {} {
            set master [srv 0 client]
            $master config set repl-diskless-sync [expr {$sync eq "diskless" ? "yes" : "no"}]
            $master config set repl-diskless-sync-delay 0
            $replica config set repl-diskless-load $load

            test "Full sync carries NO-EVICT (sync: $sync, load: $load)" {
                $replica set stale v
                $replica bless set stale no-evict

                $master set s v
                $master rpush l a b
                $master hset h f v
                $master expire h 100000
                $master set plain v
                foreach k {s l h} { $master bless set $k no-evict }
                $master select 10
                $master set other v
                $master bless set other no-evict
                $master select 9

                $replica replicaof [srv 0 host] [srv 0 port]
                wait_for_sync $replica
                wait_for_ofs_sync $master $replica
                assert_equal 1 [status $master sync_full]

                assert_equal 0 [$replica exists stale]
                assert_equal {h l s} [bless_scan_all $replica]
                assert_equal {} [$replica bless get plain]
                assert {[$replica ttl h] > 0}
                $replica select 10
                assert_equal {NO-EVICT} [$replica bless get other]
                $replica select 9
                assert_equal 4 [status $replica blessed_keys]
            }
        }
    }
}

# Replicas ignore maxmemory by default, so promotion is the first point where
# eviction runs on the replicated data.
start_server {tags {"repl" "bless" "external:skip"}} {
    set replica [srv 0 client]
    start_server {} {
        set master [srv 0 client]
        $replica replicaof [srv 0 host] [srv 0 port]
        wait_for_sync $replica

        test {A promoted replica keeps protecting blessed keys under eviction} {
            $master set precious [string repeat x 2000]
            $master bless set precious no-evict
            wait_for_ofs_sync $master $replica

            $replica replicaof no one
            $replica config set maxmemory-policy allkeys-random
            set used [status $replica used_memory]
            $replica config set maxmemory [expr {$used + 3000000}]
            for {set j 0} {$j < 4000} {incr j} {
                $replica set flood:$j [string repeat y 2000]
            }
            assert {[status $replica evicted_keys] > 0}
            assert_equal 1 [$replica exists precious]
            assert_equal {NO-EVICT} [$replica bless get precious]
            $replica config set maxmemory 0
        }
    }
}

start_cluster 1 1 {tags {"repl" "bless" "external:skip" "cluster"}} {
    test {BLESS SET and BLESS CLEAR on a cluster replica are redirected to the primary} {
        R 0 set k v
        R 0 bless set k no-evict
        wait_for_ofs_sync [Rn 0] [Rn 1]
        R 1 readonly
        assert_equal {NO-EVICT} [R 1 bless get k]
        assert_error {*MOVED*} {R 1 bless set k no-evict}
        assert_error {*MOVED*} {R 1 bless clear k no-evict}
    }
}
