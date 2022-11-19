start_server {tags {"swap string"}} {
    r config set swap-debug-evict-keys 0
    test {swap out string} {
        r set k v
        r pexpire k 200
        assert_match "*keys=1,*" [r info keyspace] 
        r evict k
        wait_key_cold r k
        after 500
        assert_match [r get k] {}
    }

    test {scan trigger cold key expire} {
        set wait_cold_time 200
        r psetex foo $wait_cold_time bar
        r evict foo
        wait_key_cold r foo
        assert_equal [lindex [r scan 1] 1] {foo}
        after $wait_cold_time
        assert_equal [lindex [r scan 1] 1] {}
        catch {[r swap object foo]} err
        assert_match {*ERR no such key*} $err
    }
} 

start_server {tags "expire"} {
    # control evict manually
    r config set swap-debug-evict-keys 0

    # TODO enable when active expire ready
    # test {cold key active expire} {
        # r psetex foo 100 bar
        # r evict foo
        # after 400
        # assert_equal [r dbsize] 0
        # assert {[rio_get_meta r foo] == ""}
    # }

    test {cold key passive expire} {
        r debug set-active-expire 0
        r psetex foo 100 bar
        r evict foo
        after 150
        assert_equal [r ttl foo] -2
        assert {[rio_get_meta r foo] == ""}
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

    test {hot key(non-dirty) active expire} {
        r psetex foo 500 bar
        r evict foo
        wait_key_cold r foo
        assert {[rio_get_meta r foo] != ""}
        assert_equal [r get foo] bar
        # wait active expire cycle to do it's job
        after 800
        assert_equal [r dbsize] 0
        assert {[rio_get_meta r foo] == ""}
    }

    test {hot key passive expire} {
        r debug set-active-expire 0
        r psetex foo 100 bar
        after 150
        assert_equal [r ttl foo] -2
        r debug set-active-expire 1
    }

    test {hot key(non-dirty) passive expire} {
        r debug set-active-expire 0

        r psetex foo 500 bar
        r evict foo
        wait_key_cold r foo
        assert {[rio_get_meta r foo] != ""}
        assert_equal [r get foo] bar
        after 600
        # trigger passive expire
        assert_equal [r ttl foo] -2
        assert_equal [r dbsize] 0
        assert {[rio_get_meta r foo] == ""}
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
