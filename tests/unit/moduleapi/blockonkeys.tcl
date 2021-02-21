set testmodule [file normalize tests/modules/blockonkeys.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test "Module client blocked on keys: Circular BPOPPUSH" {
        set rd1 [redis_deferring_client]
        set rd2 [redis_deferring_client]

        r del src dst

        $rd1 fsl.bpoppush src dst 0
        $rd2 fsl.bpoppush dst src 0
        ;# wait until clients are actually blocked
        wait_for_condition 50 100 {
            [s 0 blocked_clients] eq {2}
        } else {
            fail "Clients are not blocked"
        }

        r fsl.push src 42

        assert_equal {42} [r fsl.getall src]
        assert_equal {} [r fsl.getall dst]
    }

    test "Module client blocked on keys: Self-referential BPOPPUSH" {
        set rd1 [redis_deferring_client]

        r del src

        $rd1 fsl.bpoppush src src 0
        ;# wait until clients are actually blocked
        wait_for_condition 50 100 {
            [s 0 blocked_clients] eq {1}
        } else {
            fail "Clients are not blocked"
        }
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
        ;# wait until clients are actually blocked
        wait_for_condition 50 100 {
            [s 0 blocked_clients] eq {1}
        } else {
            fail "Clients are not blocked"
        }
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
        ;# wait until clients are actually blocked
        wait_for_condition 50 100 {
            [s 0 blocked_clients] eq {1}
        } else {
            fail "Clients are not blocked"
        }
        r fsl.push k 34
        assert_equal {34} [$rd read]
        r client kill id $cid ;# try to smoke-out client-related memory leak
    }

    test {Module client blocked on keys (with metadata): Blocked, case 2} {
        r del k
        set rd [redis_deferring_client]
        $rd fsl.bpopgt k 35 0
        ;# wait until clients are actually blocked
        wait_for_condition 50 100 {
            [s 0 blocked_clients] eq {1}
        } else {
            fail "Clients are not blocked"
        }
        r fsl.push k 33
        r fsl.push k 34
        r fsl.push k 35
        r fsl.push k 36
        assert_equal {36} [$rd read]
    }

    test {Module client blocked on keys (with metadata): Blocked, CLIENT KILL} {
        r del k
        set rd [redis_deferring_client]
        $rd client id
        set cid [$rd read]
        $rd fsl.bpopgt k 35 0
        ;# wait until clients are actually blocked
        wait_for_condition 50 100 {
            [s 0 blocked_clients] eq {1}
        } else {
            fail "Clients are not blocked"
        }
        r client kill id $cid ;# try to smoke-out client-related memory leak
    }

    test {Module client blocked on keys (with metadata): Blocked, CLIENT UNBLOCK TIMEOUT} {
        r del k
        set rd [redis_deferring_client]
        $rd client id
        set cid [$rd read]
        $rd fsl.bpopgt k 35 0
        ;# wait until clients are actually blocked
        wait_for_condition 50 100 {
            [s 0 blocked_clients] eq {1}
        } else {
            fail "Clients are not blocked"
        }
        r client unblock $cid timeout ;# try to smoke-out client-related memory leak
        assert_equal {Request timedout} [$rd read]
    }

    test {Module client blocked on keys (with metadata): Blocked, CLIENT UNBLOCK ERROR} {
        r del k
        set rd [redis_deferring_client]
        $rd client id
        set cid [$rd read]
        $rd fsl.bpopgt k 35 0
        ;# wait until clients are actually blocked
        wait_for_condition 50 100 {
            [s 0 blocked_clients] eq {1}
        } else {
            fail "Clients are not blocked"
        }
        r client unblock $cid error ;# try to smoke-out client-related memory leak
        assert_error "*unblocked*" {$rd read}
    }

    test {Module client re-blocked on keys after woke up on wrong type} {
        r del k
        set rd [redis_deferring_client]
        $rd fsl.bpop k 0
        ;# wait until clients are actually blocked
        wait_for_condition 50 100 {
            [s 0 blocked_clients] eq {1}
        } else {
            fail "Clients are not blocked"
        }
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
        # wait until client is actually blocked
        wait_for_condition 50 100 {
            [s 0 blocked_clients] eq {1}
        } else {
            fail "Client is not blocked"
        }
        r lpush k 42 squirrel banana
        assert_equal {banana squirrel 42} [$rd read]
        $rd close
    }

    test {Module client unblocks BLPOP} {
        r del k
        set rd [redis_deferring_client]
        $rd blpop k 3
        # wait until client is actually blocked
        wait_for_condition 50 100 {
            [s 0 blocked_clients] eq {1}
        } else {
            fail "Client is not blocked"
        }
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
        # Wait until client is actually blocked
        wait_for_condition 50 100 {
            [s 0 blocked_clients] eq {1}
        } else {
            fail "Client is not blocked"
        }
        # Check that RM_SignalKeyAsReady() can wake up BLPOPN
        r blockonkeys.lpush_unblock k bb cc ;# Not enough elements for BLPOPN
        r lpush k dd ee ff                  ;# Doesn't unblock module
        r blockonkeys.lpush_unblock k gg    ;# Unblocks module
        assert_equal {gg ff ee dd cc} [$rd read]
        $rd close
    }
}
