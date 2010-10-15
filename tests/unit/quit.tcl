start_server {tags {"quit"}} {
    proc format_command {args} {
        set cmd "*[llength $args]\r\n"
        foreach a $args {
            append cmd "$[string length $a]\r\n$a\r\n"
        }
        set _ $cmd
    }

    test "QUIT returns OK" {
        reconnect
        assert_equal OK [r quit]
        assert_error * {r ping}
    }

    test "Pipelined commands after QUIT must not be executed" {
        reconnect
        r write [format_command quit]
        r write [format_command set foo bar]
        r flush
        assert_equal OK [r read]
        assert_error * {r read}

        reconnect
        assert_equal {} [r get foo]
    }

    test "Pipelined commands after QUIT that exceed read buffer size" {
        reconnect
        r write [format_command quit]
        r write [format_command set foo [string repeat "x" 1024]]
        r flush
        assert_equal OK [r read]
        assert_error * {r read}

        reconnect
        assert_equal {} [r get foo]

    }
}
