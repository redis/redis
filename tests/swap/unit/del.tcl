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

