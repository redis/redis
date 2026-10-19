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

# With repl-diskless-load swapdb the replica loads the dataset into a temporary
# DB and swaps it in, which must also swap the blessed-keys index. The replica
# starts with a blessed key of its own that must leave the index.
start_server {tags {"repl" "bless" "external:skip"}} {
    set replica [srv 0 client]
    start_server {} {
        set master [srv 0 client]
        $master config set repl-diskless-sync yes
        $master config set repl-diskless-sync-delay 0
        $replica config set repl-diskless-load swapdb

        test {Diskless swapdb load swaps the NO-EVICT index} {
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
