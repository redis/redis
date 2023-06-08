set testmodule [file normalize tests/modules/blockonkeys.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test "Module client blocked on keys: Circular BPOPPUSH" {
        set rd1 [redis_deferring_client]
        set rd2 [redis_deferring_client]

        r del src dst

        $rd1 fsl.bpoppush src dst 0
        wait_for_blocked_clients_count 1

        $rd2 fsl.bpoppush dst src 0
        wait_for_blocked_clients_count 2

        r fsl.push src 42
        wait_for_blocked_clients_count 0

        assert_equal {42} [r fsl.getall src]
        assert_equal {} [r fsl.getall dst]
    }

    test "Module client blocked on keys: Self-referential BPOPPUSH" {
        set rd1 [redis_deferring_client]

        r del src

        $rd1 fsl.bpoppush src src 0
        wait_for_blocked_clients_count 1
        r fsl.push src 42

        assert_equal {42} [r fsl.getall src]
    }

    test {Module client blocked on keys (no metadata): No block} {
        r del k
        r fsl.push k 33
        r fsl.push k 34
        r fsl.bpop k 0
    } {34}

    test {Module client blocked on keys (no metadata): Timeout} {
        r del k
        set rd [redis_deferring_client]
        $rd fsl.bpop k 1
        assert_equal {Request timedout} [$rd read]
    }

    test {Module client blocked on keys (no metadata): Blocked} {
        r del k
        set rd [redis_deferring_client]
        $rd fsl.bpop k 0
        wait_for_blocked_clients_count 1
        r fsl.push k 34
        assert_equal {34} [$rd read]
    }

    test {Module client blocked on keys (with metadata): No block} {
        r del k
        r fsl.push k 34
        r fsl.bpopgt k 30 0
    } {34}

    test {Module client blocked on keys (with metadata): Timeout} {
        r del k
        set rd [redis_deferring_client]
        $rd client id
        set cid [$rd read]
        r fsl.push k 33
        $rd fsl.bpopgt k 35 1
        assert_equal {Request timedout} [$rd read]
        r client kill id $cid ;# try to smoke-out client-related memory leak
    }

    test {Module client blocked on keys (with metadata): Blocked, case 1} {
        r del k
        set rd [redis_deferring_client]
        $rd client id
        set cid [$rd read]
        r fsl.push k 33
        $rd fsl.bpopgt k 33 0
        wait_for_blocked_clients_count 1
        r fsl.push k 34
        assert_equal {34} [$rd read]
        r client kill id $cid ;# try to smoke-out client-related memory leak
    }

    test {Module client blocked on keys (with metadata): Blocked, case 2} {
        r del k
        r fsl.push k 32
        set rd [redis_deferring_client]
        $rd fsl.bpopgt k 35 0
        wait_for_blocked_clients_count 1
        r fsl.push k 33
        r fsl.push k 34
        r fsl.push k 35
        r fsl.push k 36
        assert_equal {36} [$rd read]
    }

    test {Module client blocked on keys (with metadata): Blocked, DEL} {
        r del k
        r fsl.push k 32
        set rd [redis_deferring_client]
        $rd fsl.bpopgt k 35 0
        wait_for_blocked_clients_count 1
        r del k
        assert_error {*UNBLOCKED key no longer exists*} {$rd read}
    }

    test {Module client blocked on keys (with metadata): Blocked, FLUSHALL} {
        r del k
        r fsl.push k 32
        set rd [redis_deferring_client]
        $rd fsl.bpopgt k 35 0
        wait_for_blocked_clients_count 1
        r flushall
        assert_error {*UNBLOCKED key no longer exists*} {$rd read}
    }

    test {Module client blocked on keys (with metadata): Blocked, SWAPDB, no key} {
        r select 9
        r del k
        r fsl.push k 32
        set rd [redis_deferring_client]
        $rd fsl.bpopgt k 35 0
        wait_for_blocked_clients_count 1
        r swapdb 0 9
        assert_error {*UNBLOCKED key no longer exists*} {$rd read}
    }

    test {Module client blocked on keys (with metadata): Blocked, SWAPDB, key exists, case 1} {
        ;# Key exists on other db, but wrong type
        r flushall
        r select 9
        r fsl.push k 32
        r select 0
        r lpush k 38
        r select 9
        set rd [redis_deferring_client]
        $rd fsl.bpopgt k 35 0
        wait_for_blocked_clients_count 1
        r swapdb 0 9
        assert_error {*UNBLOCKED key no longer exists*} {$rd read}
        r select 9
    }

    test {Module client blocked on keys (with metadata): Blocked, SWAPDB, key exists, case 2} {
        ;# Key exists on other db, with the right type, but the value doesn't allow to unblock
        r flushall
        r select 9
        r fsl.push k 32
        r select 0
        r fsl.push k 34
        r select 9
        set rd [redis_deferring_client]
        $rd fsl.bpopgt k 35 0
        wait_for_blocked_clients_count 1
        r swapdb 0 9
        assert_equal {1} [s 0 blocked_clients]
        r fsl.push k 38
        assert_equal {38} [$rd read]
        r select 9
    }

    test {Module client blocked on keys (with metadata): Blocked, SWAPDB, key exists, case 3} {
        ;# Key exists on other db, with the right type, the value allows to unblock
        r flushall
        r select 9
        r fsl.push k 32
        r select 0
        r fsl.push k 38
        r select 9
        set rd [redis_deferring_client]
        $rd fsl.bpopgt k 35 0
        wait_for_blocked_clients_count 1
        r swapdb 0 9
        assert_equal {38} [$rd read]
        r select 9
    }

    test {Module client blocked on keys (with metadata): Blocked, CLIENT KILL} {
        r del k
        r fsl.push k 32
        set rd [redis_deferring_client]
        $rd client id
        set cid [$rd read]
        $rd fsl.bpopgt k 35 0
        wait_for_blocked_clients_count 1
        r client kill id $cid ;# try to smoke-out client-related memory leak
    }

    test {Module client blocked on keys (with metadata): Blocked, CLIENT UNBLOCK TIMEOUT} {
        r del k
        r fsl.push k 32
        set rd [redis_deferring_client]
        $rd client id
        set cid [$rd read]
        $rd fsl.bpopgt k 35 0
        wait_for_blocked_clients_count 1
        r client unblock $cid timeout ;# try to smoke-out client-related memory leak
        assert_equal {Request timedout} [$rd read]
    }

    test {Module client blocked on keys (with metadata): Blocked, CLIENT UNBLOCK ERROR} {
        r del k
        r fsl.push k 32
        set rd [redis_deferring_client]
        $rd client id
        set cid [$rd read]
        $rd fsl.bpopgt k 35 0
        wait_for_blocked_clients_count 1
        r client unblock $cid error ;# try to smoke-out client-related memory leak
        assert_error "*unblocked*" {$rd read}
    }

    test {Module client blocked on keys, no timeout CB, CLIENT UNBLOCK TIMEOUT} {
        r del k
        set rd [redis_deferring_client]
        $rd client id
        set cid [$rd read]
        $rd fsl.bpop k 0 NO_TO_CB
        wait_for_blocked_clients_count 1
        assert_equal [r client unblock $cid timeout] {0}
        $rd close
    }

    test {Module client blocked on keys, no timeout CB, CLIENT UNBLOCK ERROR} {
        r del k
        set rd [redis_deferring_client]
        $rd client id
        set cid [$rd read]
        $rd fsl.bpop k 0 NO_TO_CB
        wait_for_blocked_clients_count 1
        assert_equal [r client unblock $cid error] {0}
        $rd close
    }

    test {Module client re-blocked on keys after woke up on wrong type} {
        r del k
        set rd [redis_deferring_client]
        $rd fsl.bpop k 0
        wait_for_blocked_clients_count 1
        r lpush k 12
        r lpush k 13
        r lpush k 14
        r del k
        r fsl.push k 34
        assert_equal {34} [$rd read]
        assert_equal {1} [r get fsl_wrong_type] ;# first lpush caused one wrong-type wake-up
    }

    test {Module client blocked on keys woken up by LPUSH} {
        r del k
        set rd [redis_deferring_client]
        $rd blockonkeys.popall k
        wait_for_blocked_clients_count 1
        r lpush k 42 squirrel banana
        assert_equal {banana squirrel 42} [$rd read]
        $rd close
    }

    test {Module client unblocks BLPOP} {
        r del k
        set rd [redis_deferring_client]
        $rd blpop k 3
        wait_for_blocked_clients_count 1
        r blockonkeys.lpush k 42
        assert_equal {k 42} [$rd read]
        $rd close
    }

    test {Module unblocks module blocked on non-empty list} {
        r del k
        r lpush k aa
        # Module client blocks to pop 5 elements from list
        set rd [redis_deferring_client]
        $rd blockonkeys.blpopn k 5
        wait_for_blocked_clients_count 1
        # Check that RM_SignalKeyAsReady() can wake up BLPOPN
        r blockonkeys.lpush_unblock k bb cc ;# Not enough elements for BLPOPN
        r lpush k dd ee ff                  ;# Doesn't unblock module
        r blockonkeys.lpush_unblock k gg    ;# Unblocks module
        assert_equal {gg ff ee dd cc} [$rd read]
        $rd close
    }
    
    test {Module explicit unblock when blocked on keys} {
        r del k
        r set somekey someval
        # Module client blocks to pop 5 elements from list
        set rd [redis_deferring_client]
        $rd blockonkeys.blpopn_or_unblock k 5 0
        wait_for_blocked_clients_count 1
        # will now cause the module to trigger pop but instead will unblock the client from the reply_callback
        r lpush k dd
        # we should still get unblocked as the command should not reprocess
        wait_for_blocked_clients_count 0
        assert_equal {Action aborted} [$rd read]
        $rd get somekey
        assert_equal {someval} [$rd read]
        $rd close
    }

    set master [srv 0 client]
    set master_host [srv 0 host]
    set master_port [srv 0 port]
    start_server [list overrides [list loadmodule "$testmodule"]] {
        set replica [srv 0 client]
        set replica_host [srv 0 host]
        set replica_port [srv 0 port]

        # Start the replication process...
        $replica replicaof $master_host $master_port
        wait_for_sync $replica

        test {WAIT command on module blocked client on keys} {
            set rd [redis_deferring_client -1]
            $rd set x y
            $rd read

            pause_process [srv 0 pid]

            $master del k
            $rd fsl.bpop k 0
            wait_for_blocked_client -1
            $master fsl.push k 34
            $master fsl.push k 35
            assert_equal {34} [$rd read]

            assert_equal [$master wait 1 1000] 0
            resume_process [srv 0 pid]
            assert_equal [$master wait 1 1000] 1
            $rd close
            assert_equal {35} [$replica fsl.getall k]
        }
    }

}
