start_server {tags "del"} {
    r config set swap-debug-evict-keys 0

    test {delete hot key} {
        r psetex foo 100 bar
        assert_match {*keys=1,*} [r info keyspace]
        r del foo
        after 110
        assert_equal [r dbsize] 0
    }

    test {delete non-dirty hot key} {
        r set foo bar
        assert_match {*keys=1,*} [r info keyspace]

        r swap.evict foo
        wait_key_cold r foo
        assert {[rio_get_meta r foo] != ""}

        assert_equal [r get foo] bar
        assert {[rio_get_meta r foo] != ""}

        r del foo
        after 110
        assert_equal [r dbsize] 0
        assert {[rio_get_meta r foo] == ""}
    }

    test {delete cold key} {
        r psetex foo 1000 bar
        r swap.evict foo
        wait_key_cold r foo
        assert {[rio_get_meta r foo] != ""}
        r del foo
        after 110
        assert_equal [r dbsize] 0
        assert {[rio_get_meta r foo] == ""}
    }

    test {del expired hot key} {
        r debug set-active-expire 0
        r psetex foo 100 bar
        after 110
        assert_equal [r dbsize] 1
        assert_match {*keys=1,*} [r info keyspace] 
        assert_equal [r del foo] 0
        after 100
        assert_equal [r dbsize] 0
        r debug set-active-expire 1
    }

    test {del expired cold key} {
        r debug set-active-expire 0
        r psetex foo 100 bar
        r swap.evict foo
        after 110
        assert {[rio_get_meta r foo] != ""}
        assert_equal [r del foo] 0
        after 110
        assert_equal [r dbsize] 0
        assert {[rio_get_meta r foo] == ""}
        r debug set-active-expire 1
    }
}

start_server {tags {"skip_unnecessary_rocksdb_del"}} {
    proc scan_all_keys {r} {
        set keys {}
        set cursor 0
        while {1} {
            set res [$r scan $cursor]
            set cursor [lindex $res 0]
            lappend keys {*}[split [lindex $res 1] " "]
            if {$cursor == 0} {
                break
            }
        }
        set _ $keys
    }

    r config set swap-debug-evict-keys 0
    test {delete keys and keys deleted completed} {
        set load_handle0 [start_bg_complex_data [srv 0 host] [srv 0 port] 0 1000000]
        after 10000
        stop_bg_complex_data $load_handle0
        wait_load_handlers_disconnected
        set keys [scan_all_keys r]
        foreach key $keys {
            r del {*}$key
            assert_match [r get $key] {}
        }
        assert_equal 0 [r dbsize]
    }

    test {expire keys and keys deleted completed} {
        set load_handle0 [start_bg_complex_data [srv 0 host] [srv 0 port] 0 1000000]
        after 10000
        stop_bg_complex_data $load_handle0
        wait_load_handlers_disconnected
        set keys [scan_all_keys r]
        foreach key $keys {
            r pexpire {*}$key 1
        }
        wait_for_condition 50 100 {
            [r dbsize] == 0
        } else {
            fail "key wasn't expired"
        }
        foreach key $keys {
            assert_match [r get $key] {}
        }
    }
}

