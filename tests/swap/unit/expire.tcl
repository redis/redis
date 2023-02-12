proc scan_all_keys {r} {
    set keys {}
    set cursor 0
    while {1} {
        set res [$r scan $cursor]
        set cursor [lindex $res 0]
        append keys [lindex $res 1]
        if {$cursor == 0} {
            break
        }
    }
    set _ $keys
}
# start_server {tags {"scan all keys"}} {
    # for {set i 0} {$i < 30} {incr i} {
        # r set $i $i
    # }
    # wait_keyspace_cold r
    # puts "keys-1: [scan_all_keys r]"
    # puts "keys-2: [scan_all_keys r]"
    # press_enter_to_continue
# }

start_server {tags {"swap string"}} {
    r config set swap-debug-evict-keys 0
    test {swap out string} {
        r set k v
        r pexpire k 200
        assert_match "*keys=1,*" [r info keyspace]
        r swap.evict k
        wait_key_cold r k
        after 500
        assert_match [r get k] {}
    }

    test {scan trigger cold key expire} {
        set wait_cold_time 200
        r psetex foo $wait_cold_time bar
        r swap.evict foo
        wait_key_cold r foo
        assert_equal [scan_all_keys r] {foo}
        after $wait_cold_time
        assert_equal [scan_all_keys r] {}
        #puts [r config get port]
        #press_enter_to_continue
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
        # r swap.evict foo
        # after 400
        # assert_equal [r dbsize] 0
        # assert {[rio_get_meta r foo] == ""}
    # }

    test {cold key passive expire} {
        r debug set-active-expire 0
        r psetex foo 100 bar
        r swap.evict foo
        after 150
        assert_equal [r ttl foo] -2
        assert {[rio_get_meta r foo] == ""}
        r debug set-active-expire 1
    }

    test {cold key expire scaned} {
        r debug set-active-expire 0
        r psetex foo 100 bar
        r swap.evict foo
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
        r swap.evict foo
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
        r swap.evict foo
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
        r swap.evict foo
        after 150
        set res [r scan 0]
        set next_cursor [lindex $res 0]
        assert_equal [llength [lindex $res 1]] 0
        set res [r scan $next_cursor]
        assert_equal [llength [lindex $res 1]] 0
        r debug set-active-expire 1
    }
}
