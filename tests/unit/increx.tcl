start_server {tags {"increx"}} {
    test {Test increx command} {
        r del foo
        r set foo 100
        r increx foo 1
        set mttl [r pttl foo]
        assert {$mttl > 900 && $mttl <= 1000}
        r get foo
    } {101}

    test {Test increx ttl command} {
        r del foo
        r increx foo 1
        set mttl [r pttl foo]
        assert {$mttl > 900 && $mttl <= 1000}
        r get foo
    } {1}

    test {Test increx on key with ttl} {
        r del foo
        r set foo 100
        r expire foo 10
        r debug sleep 5
        r increx foo 10
        set mttl [r pttl foo]
        assert {$mttl <= 50000}
        r get foo
    } {101}

    test {HSET/HLEN - Small hash creation} {
        array set smallhash {}
        for {set i 0} {$i < 8} {incr i} {
            set key [randstring 0 8 alpha]
            set val [randstring 0 8 alpha]
            if {[info exists smallhash($key)]} {
                incr i -1
                continue
            }
            r hset smallhash1 $key $val
            set smallhash($key) $val
        }
        list [r hlen smallhash1]
    } {8}

    test {HSET/HLEN - Small hash creation} {
        array set smallhash {}
        for {set i 0} {$i < 8} {incr i} {
            set key [randstring 0 8 alpha]
            set val [randstring 0 8 alpha]
            if {[info exists smallhash($key)]} {
                incr i -1
                continue
            }
            r hset smallhash2 $key $val
            set smallhash($key) $val
        }
        list [r hlen smallhash2]
    } {8}

    test {HSET/HLEN - Small hash creation} {
        array set smallhash {}
        for {set i 0} {$i < 8} {incr i} {
            set key [randstring 0 8 alpha]
            set val [randstring 0 8 alpha]
            if {[info exists smallhash($key)]} {
                incr i -1
                continue
            }
            r hset smallhash3 $key $val
            set smallhash($key) $val
        }
        list [r hlen smallhash3]
    } {8}

    start_server {} {
        test { increx passive expire works on non read only slave } {
           r slaveof [srv -1 host] [srv -1 port]
           wait_for_condition 50 100 {
               [s role] eq {slave} &&
               [string match {*master_link_status:up*} [r info replication]]
           } else {
               fail "Can't turn the instance into a slave"
           }

           r -1 debug set-active-expire 0
           r config set slave-read-only no
           assert_equal [r config get slave-read-only] "slave-read-only no"
           r -1 set foo 10000
           r -1 increx foo 10
           assert_equal [r -1 get foo] 10001
           after 12000
           assert_equal [r ttl foo] -2
           assert_equal [r get foo] ""
           r increx foo 60
           assert_equal [r ttl foo] 60
           r get foo
        } {1}
    }
}

