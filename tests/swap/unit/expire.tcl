start_server {tags {"swap string"}} {
    r config set debug-evict-keys 0
    test {swap out string} {
        r set k v
        r pexpire k 200
        assert_match "*keys=1,evicts=0*" [r info keyspace] 
        r evict k
        wait_for_condition 10 50 {
            [string match "*keys=0,evicts=1,expires=1*" [r info keyspace]]
        } else {
            fail "evict fail"
        } 
        after 250
        assert_equal {# Keyspace} [string trim [r info keyspace]]
        assert_match [r get k] {}
    }
} 

start_server {tags "expire"} {
    # control evict manually
    r config set debug-evict-keys 0

    test {cold key active expire} {
        r psetex foo 100 bar
        r evict foo
        after 400
        assert_equal [r dbsize] 0
    }

    test {cold key passive expire} {
        r debug set-active-expire 0
        r psetex foo 100 bar
        r evict foo
        after 150
        assert_equal [r ttl foo] -2
        r debug set-active-expire 1
    }

    test {cold key expire scaned} {
        r debug set-active-expire 0
        r psetex foo 100 bar
        r evict foo
        after 150
        set res [r scan 0]
        assert_equal [lindex $res 0] 1
        set res [r scan 1]
        assert_equal [llength [lindex $res 1]] 0
        r debug set-active-expire 1
    }

    test {hot key active expire} {
        r psetex foo 100 bar
        after 400
        assert_equal [r dbsize] 0
    }

    test {hot key passive expire} {
        r debug set-active-expire 0
        r psetex foo 100 bar
        after 150
        assert_equal [r ttl foo] -2
        r debug set-active-expire 1
    }

    test {hot key expire scaned} {
        r debug set-active-expire 0
        r psetex foo 100 bar
        r evict foo
        after 150
        set res [r scan 0]
        assert_equal [lindex $res 0] 1
        assert_equal [llength [lindex $res 1]] 0
        set res [r scan 1]
        assert_equal [llength [lindex $res 1]] 0
        r debug set-active-expire 1
    }
}
