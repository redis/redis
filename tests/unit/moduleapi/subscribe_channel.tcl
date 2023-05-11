# subscribe_channel.c defines a module supporting two commands i.e.
# subscribe_to_channel CLASSIC|SHARD channel_name
# unsubscribe_from_channel CLASSIC|SHARD channel_name
#
# The module on load subscribes to `event` channel and `shardevent` sharded channel
# and register a callback which listens to message and upon receiving "clear" message
# on either channel, the module executes a FLUSHALL command. The module also deregisters
# the callback on receiving "unsubscribe" message on the above channel(s).

set testmodule [file normalize tests/modules/subscribe_channel.so]

tags "modules" {
    foreach type {classic shard} {
        start_server [list overrides [list loadmodule "$testmodule"]] {
            if {$type == "classic"} {
                set channel "event"
                set command "PUBLISH"
            } elseif {$type == "shard"} {
                set channel "shardevent"
                set command "SPUBLISH"
            }

            test {Test message received by module from subscribed channel} {
                r set foo bar
                assert_equal {0} [r $command $channel clear]
                assert_equal 0 [r dbsize]
            }

            test {Test message not handled by module from a random channel} {
                r set foo bar
                assert_equal {0} [r $command event1 clear]
                assert_equal 1 [r dbsize]
            }

            test {Unsubscribe from channel event, message should no longer be received by module} {
                # module is listening to "event" channel on load
                r subscribech.unsubscribe_from_channel $type $channel
                assert_equal 1 [r dbsize]
                assert_equal {0} [r $command $channel clear]
                assert_equal 1 [r dbsize]

                # module is listening to $type channel again
                r subscribech.subscribe_to_channel $type $channel
                assert_equal {0} [r $command $channel clear]
                assert_equal 0 [r dbsize]

                # try unsubscribe from non subscribed channel
                r subscribech.unsubscribe_from_channel $type event1
            }

            test {Unsubscribe from channel, via trigger on message} {
                r subscribech.subscribe_to_channel $type $channel
                r set foo bar

                # verify the module is still listening to events
                assert_equal 1 [r dbsize]
                assert_equal {0} [r $command $channel clear]
                assert_equal 0 [r dbsize]

                r set foo bar
                r $command $channel unsubscribe

                # verify the module is no longer listening to events
                assert_equal {0} [r $command $channel clear]
                assert_equal 1 [r dbsize]
            }

            test "Unload the module - subscribech" {
                assert_equal {OK} [r module unload subscribech]

                # verify no side effect after module unload.
                assert_equal {0} [r $command $channel clear]
                assert_equal 1 [r dbsize]
            }
        }
    }
}
