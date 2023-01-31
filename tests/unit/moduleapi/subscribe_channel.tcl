set testmodule [file normalize tests/modules/subscribe_channel.so]

tags "modules" {
    start_server [list overrides [list loadmodule "$testmodule"]] {

        test {Test message received by module from subscribed channel} {
            r set foo bar
            assert_equal {0} [r publish event clear]
            assert_equal 0 [r dbsize]
        }

        test {Test message not received by module from a random channel} {
            r set foo bar
            assert_equal {0} [r publish event1 clear]
            assert_equal 1 [r dbsize]
        }

        test {Unsubscribe from channelMessage no longer received by module} {
            # module is listening to "event" channel
            r publish event unsubscribe
            assert_equal 1 [r dbsize]
            assert_equal {0} [r publish event clear]
            assert_equal 1 [r dbsize]
        }

        test "Unload the module - subscribech" {
            assert_equal {OK} [r module unload subscribech]
        }
    }
}