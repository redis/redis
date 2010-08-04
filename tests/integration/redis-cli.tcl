start_server {tags {"cli"}} {
    proc open_cli {} {
        set ::env(TERM) dumb
        set fd [open [format "|src/redis-cli -p %d -n 9" [srv port]] "r+"]
        fconfigure $fd -buffering none
        fconfigure $fd -blocking false
        fconfigure $fd -translation binary
        assert_equal "redis> " [read_cli $fd]
        set _ $fd
    }

    proc close_cli {fd} {
        close $fd
    }

    proc read_cli {fd} {
        set buf [read $fd]
        while {[string length $buf] == 0} {
            # wait some time and try again
            after 10
            set buf [read $fd]
        }
        set _ $buf
    }

    proc write_cli {fd buf} {
        puts $fd $buf
        flush $fd
    }

    proc run_command {fd cmd} {
        write_cli $fd $cmd
        set lines [split [read_cli $fd] "\n"]
        assert_equal "redis> " [lindex $lines end]
        join [lrange $lines 0 end-1] "\n"
    }

    proc test_interactive_cli {name code} {
        set fd [open_cli]
        test "Interactive CLI: $name" $code
        close_cli $fd
    }

    proc run_cli {args} {
        set fd [open [format "|src/redis-cli -p %d -n 9 $args" [srv port]] "r"]
        fconfigure $fd -buffering none
        fconfigure $fd -translation binary
        set resp [read $fd 1048576]
        close $fd
        set _ $resp
    }

    proc test_noninteractive_cli {name code} {
        test "Non-interactive CLI: $name" $code
    }

    test_interactive_cli "INFO response should be printed raw" {
        set lines [split [run_command $fd info] "\n"]
        foreach line $lines {
            assert [regexp {^[a-z0-9_]+:[a-z0-9_]+} $line]
        }
    }

    test_interactive_cli "Status reply" {
        assert_equal "OK" [run_command $fd "set key foo"]
    }

    test_interactive_cli "Integer reply" {
        assert_equal "(integer) 1" [run_command $fd "incr counter"]
    }

    test_interactive_cli "Bulk reply" {
        r set key foo
        assert_equal "\"foo\"" [run_command $fd "get key"]
    }

    test_interactive_cli "Multi-bulk reply" {
        r rpush list foo
        r rpush list bar
        assert_equal "1. \"foo\"\n2. \"bar\"" [run_command $fd "lrange list 0 -1"]
    }

    test_interactive_cli "Parsing quotes" {
        assert_equal "OK" [run_command $fd "set key \"bar\""]
        assert_equal "bar" [r get key]
        assert_equal "OK" [run_command $fd "set key \" bar \""]
        assert_equal " bar " [r get key]
        assert_equal "OK" [run_command $fd "set key \"\\\"bar\\\"\""]
        assert_equal "\"bar\"" [r get key]
        assert_equal "OK" [run_command $fd "set key \"\tbar\t\""]
        assert_equal "\tbar\t" [r get key]

        # invalid quotation
        assert_equal "Invalid argument(s)" [run_command $fd "get \"\"key"]
        assert_equal "Invalid argument(s)" [run_command $fd "get \"key\"x"]

        # quotes after the argument are weird, but should be allowed
        assert_equal "OK" [run_command $fd "set key\"\" bar"]
        assert_equal "bar" [r get key]
    }

    test_noninteractive_cli "Status reply" {
        assert_equal "OK\n" [run_cli set key bar]
        assert_equal "bar" [r get key]
    }

    test_noninteractive_cli "Integer reply" {
        r del counter
        assert_equal "(integer) 1\n" [run_cli incr counter]
    }

    test_noninteractive_cli "Bulk reply" {
        r set key "tab\tnewline\n"
        assert_equal "\"tab\\tnewline\\n\"\n" [run_cli get key]
    }

    test_noninteractive_cli "Multi-bulk reply" {
        r del list
        r rpush list foo
        r rpush list bar
        assert_equal "1. \"foo\"\n2. \"bar\"\n" [run_cli lrange list 0 -1]
    }
}
