set testmodule [file normalize tests/modules/publish.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test {PUBLISH and SPUBLISH via a module} {
        set rd1 [redis_deferring_client]
        set rd2 [redis_deferring_client]

        assert_equal {1} [ssubscribe $rd1 {chan1}]
        assert_equal {1} [subscribe $rd2 {chan1}]
        assert_equal 1 [r publish.shard chan1 hello]
        assert_equal 1 [r publish.classic chan1 world]
        assert_equal {smessage chan1 hello} [$rd1 read]
        assert_equal {message chan1 world} [$rd2 read]
        $rd1 close
        $rd2 close
    }

    test {module publish to self with multi message} {
        r hello 3
        r subscribe foo

        # published message comes after the response of the command that issued it.
        assert_equal [r publish.classic_multi foo bar vaz] {1 1}
        assert_equal [r read] {message foo bar}
        assert_equal [r read] {message foo vaz}

        r unsubscribe foo
        r hello 2
        set _ ""
    } {} {resp3}

}
