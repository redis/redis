set testmodule [file normalize tests/modules/publish.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test {PUBLISH and SPUBLISH via a module} {
        set rd1 [redis_deferring_client]
        set rd2 [redis_deferring_client]

        assert_equal {1} [ssubscribe $rd1 {chan1}]
        assert_equal {1} [subscribe $rd2 {chan1}]
        assert_equal 1 [r publish.classic chan1 hello]
        assert_equal 1 [r publish.shard chan1 hello]
        assert_equal "chan1 1" [r pubsub shardnumsub chan1]
        assert_equal "chan1 1" [r pubsub numsub chan1]
        assert_equal "chan1" [r pubsub shardchannels]
        assert_equal "chan1" [r pubsub channels]
    }
}
