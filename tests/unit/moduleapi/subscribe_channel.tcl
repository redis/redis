set testmodule [file normalize tests/modules/subscribe_channel.so]

tags "modules" {
    start_server [list overrides [list loadmodule "$testmodule"]] {

        test {Test subscribe channel} {
            r set foo bar
            assert_equal {0} [r publish event clear]
            assert_equal "" [r keys *]
        }

        test {Test unsubscribe channel} {
            r set foo bar
            assert_equal "foo" [r keys *]
            assert_equal {0} [r publish event unsubscribe]
            assert_equal {0} [r publish event clear]
            assert_equal "foo" [r keys *]
        }

    }
}