set testmodule [file normalize tests/modules/blockonkeys.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test {Module client blocked on keys (no metadata): No block} {
        r del k
        r fsl.push k 33
        r fsl.push k 34
        r fsl.bpop2 k 0
    } {34 33}

    test {Module client blocked on keys (no metadata): Timeout} {
        r del k
        set rd [redis_deferring_client]
        r fsl.push k 33
        $rd fsl.bpop2 k 1
        assert_equal {Request timedout} [$rd read]
    }

    test {Module client blocked on keys (no metadata): Blocked, case 1} {
        r del k
        set rd [redis_deferring_client]
        r fsl.push k 33
        $rd fsl.bpop2 k 0
        r fsl.push k 34
        assert_equal {34 33} [$rd read]
    }

    test {Module client blocked on keys (no metadata): Blocked, case 2} {
        r del k
        set rd [redis_deferring_client]
        r fsl.push k 33
        r fsl.push k 34
        $rd fsl.bpop2 k 0
        assert_equal {34 33} [$rd read]
    }

    test {Module client blocked on keys (with metadata): No block} {
        r del k
        r fsl.push k 34
        r fsl.bpopgt k 30 0
    } {34}

    test {Module client blocked on keys (with metadata): Timeout} {
        r del k
        set rd [redis_deferring_client]
        r fsl.push k 33
        $rd fsl.bpopgt k 35 1
        assert_equal {Request timedout} [$rd read]
    }

    test {Module client blocked on keys (with metadata): Blocked, case 1} {
        r del k
        set rd [redis_deferring_client]
        r fsl.push k 33
        $rd fsl.bpopgt k 33 0
        r fsl.push k 34
        assert_equal {34} [$rd read]
    }

    test {Module client blocked on keys (with metadata): Blocked, case 2} {
        r del k
        set rd [redis_deferring_client]
        $rd fsl.bpopgt k 35 0
        r fsl.push k 33
        r fsl.push k 34
        r fsl.push k 35
        r fsl.push k 36
        assert_equal {36} [$rd read]
    }

    test {Module client blocked on keys does not wake up on wrong type} {
        r del k
        set rd [redis_deferring_client]
        $rd fsl.bpop2 k 0
        r lpush k 12
        r lpush k 13
        r lpush k 14
        r del k
        r fsl.push k 33
        r fsl.push k 34
        assert_equal {34 33} [$rd read]
    }
}
