start_server {tags "del"} {
    # control evict manually
    r config set debug-evict-keys 0

    test {delete hot key} {
        r psetex foo 100 bar
        assert_match {*keys=1,evicts=0*} [r info keyspace]
        r del foo
        after 110
        assert_equal [r dbsize] 0
    }

    test {delete non-dirty hot key} {
        r set foo bar
        assert_match {*keys=1,evicts=0*} [r info keyspace]

        r evict foo
        wait_for_condition 50 100 {
            [string match {*keys=0,evicts=1*} [r info keyspace]]
        } else {
            fail "evict key failed."
        }
        assert {[llength [r swap rio-get [r swap encode-key K foo]]] > 0}

        assert_equal [r get foo] bar
        assert {[llength [r swap rio-get [r swap encode-key K foo]]] > 0}

        r del foo
        after 100
        assert_equal [r dbsize] 0
        assert {[llength [r swap rio-get [r swap encode-key K foo]]] == 0}
    }

    test {delete cold key} {
        r psetex foo 100 bar
        r evict foo
        wait_for_condition 50 100 {
            [string match {*keys=0,evicts=1*} [r info keyspace]]
        } else {
            fail "evict key failed."
        }
        assert {[llength [r swap rio-get [r swap encode-key K foo]]] > 0}
        r del foo
        after 100
        assert_equal [r dbsize] 0
        assert {[llength [r swap rio-get [r swap encode-key K foo]]] == 0}
    }

    test {del expired hot key} {
        r debug set-active-expire 0
        r psetex foo 100 bar
        after 110
        assert_equal [r dbsize] 1
        assert_match {*keys=1,evicts=0*} [r info keyspace] 
        assert_equal [r del foo] 0
        after 100
        assert_equal [r dbsize] 0
        r debug set-active-expire 1
    }

    test {del expired cold key} {
        r debug set-active-expire 0
        r psetex foo 100 bar
        r evict foo
        after 100
        assert_equal [r dbsize] 1
        assert_match {*keys=0,evicts=1*} [r info keyspace] 
        assert {[llength [r swap rio-get [r swap encode-key K foo]]] > 0}
        assert_equal [r del foo] 0
        after 100
        assert_equal [r dbsize] 0
        assert {[llength [r swap rio-get [r swap encode-key K foo]]] == 0}
        r debug set-active-expire 1
    }
}

