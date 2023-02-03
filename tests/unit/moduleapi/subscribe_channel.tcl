set testmodule [file normalize tests/modules/subscribe_channel.so]

tags "modules" {
    start_server [list overrides [list loadmodule "$testmodule"]] {

        test {Test message received by module from subscribed channel} {
            r set foo bar
            assert_equal {0} [r publish event clear]
            assert_equal 0 [r dbsize]
        }

        test {Test message not handled by module from a random channel} {
            r set foo bar
            assert_equal {0} [r publish event1 clear]
            assert_equal 1 [r dbsize]
        }

        test {Unsubscribe from channel event, message should no longer be received by module} {
            assert_equal [r subscribech.list] [list "event"]

            # module is listening to "event" channel on load
            r subscribech.unsubscribe_from_channel event
            assert_equal 1 [r dbsize]
            assert_equal {0} [r publish event clear]
            assert_equal 1 [r dbsize]
            assert_equal [r subscribech.list] []

            # module is listening to "event" channel again
            r subscribech.subscribe_to_channel event
            r subscribech.subscribe_to_channel event1
            assert_equal {0} [r publish event clear]
            assert_equal 0 [r dbsize]

            assert_equal [r subscribech.list] [list "event" "event1"]
        }

        test "Unload the module - subscribech" {
            assert_equal {OK} [r module unload subscribech]
        }
    }
}