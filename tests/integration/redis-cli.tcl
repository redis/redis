source tests/support/cli.tcl

start_server {tags {"cli"}} {
    proc open_cli {{opts "-n 9"} {infile ""}} {
        set ::env(TERM) dumb
        set cmdline [rediscli [srv host] [srv port] $opts]
        if {$infile ne ""} {
            set cmdline "$cmdline < $infile"
            set mode "r"
        } else {
            set mode "r+"
        }
        set fd [open "|$cmdline" $mode]
        fconfigure $fd -buffering none
        fconfigure $fd -blocking false
        fconfigure $fd -translation binary
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

    # Helpers to run tests in interactive mode

    proc format_output {output} {
        set _ [string trimright [regsub -all "\r" $output ""] "\n"]
    }

    proc run_command {fd cmd} {
        write_cli $fd $cmd
        set _ [format_output [read_cli $fd]]
    }

    proc test_interactive_cli {name code} {
        set ::env(FAKETTY) 1
        set fd [open_cli]
        test "Interactive CLI: $name" $code
        close_cli $fd
        unset ::env(FAKETTY)
    }

    # Helpers to run tests where stdout is not a tty
    proc write_tmpfile {contents} {
        set tmp [tmpfile "cli"]
        set tmpfd [open $tmp "w"]
        puts -nonewline $tmpfd $contents
        close $tmpfd
        set _ $tmp
    }

    proc _run_cli {opts args} {
        set cmd [rediscli [srv host] [srv port] [list -n 9 {*}$args]]
        foreach {key value} $opts {
            if {$key eq "pipe"} {
                set cmd "sh -c \"$value | $cmd\""
            }
            if {$key eq "path"} {
                set cmd "$cmd < $value"
            }
        }

        set fd [open "|$cmd" "r"]
        fconfigure $fd -buffering none
        fconfigure $fd -translation binary
        set resp [read $fd 1048576]
        close $fd
        set _ [format_output $resp]
    }

    proc run_cli {args} {
        _run_cli {} {*}$args
    }

    proc run_cli_with_input_pipe {cmd args} {
        _run_cli [list pipe $cmd] -x {*}$args
    }

    proc run_cli_with_input_file {path args} {
        _run_cli [list path $path] -x {*}$args
    }

    proc test_nontty_cli {name code} {
        test "Non-interactive non-TTY CLI: $name" $code
    }

    # Helpers to run tests where stdout is a tty (fake it)
    proc test_tty_cli {name code} {
        set ::env(FAKETTY) 1
        test "Non-interactive TTY CLI: $name" $code
        unset ::env(FAKETTY)
    }

    test_interactive_cli "INFO response should be printed raw" {
        set lines [split [run_command $fd info] "\n"]
        foreach line $lines {
            if {![regexp {^$|^#|^[^#:]+:} $line]} {
                fail "Malformed info line: $line"
            }
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
        assert_equal "1) \"foo\"\n2) \"bar\"" [run_command $fd "lrange list 0 -1"]
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

    test_tty_cli "Status reply" {
        assert_equal "OK" [run_cli set key bar]
        assert_equal "bar" [r get key]
    }

    test_tty_cli "Integer reply" {
        r del counter
        assert_equal "(integer) 1" [run_cli incr counter]
    }

    test_tty_cli "Bulk reply" {
        r set key "tab\tnewline\n"
        assert_equal "\"tab\\tnewline\\n\"" [run_cli get key]
    }

    test_tty_cli "Multi-bulk reply" {
        r del list
        r rpush list foo
        r rpush list bar
        assert_equal "1) \"foo\"\n2) \"bar\"" [run_cli lrange list 0 -1]
    }

    test_tty_cli "Read last argument from pipe" {
        assert_equal "OK" [run_cli_with_input_pipe "echo foo" set key]
        assert_equal "foo\n" [r get key]
    }

    test_tty_cli "Read last argument from file" {
        set tmpfile [write_tmpfile "from file"]
        assert_equal "OK" [run_cli_with_input_file $tmpfile set key]
        assert_equal "from file" [r get key]
        file delete $tmpfile
    }

    test_nontty_cli "Status reply" {
        assert_equal "OK" [run_cli set key bar]
        assert_equal "bar" [r get key]
    }

    test_nontty_cli "Integer reply" {
        r del counter
        assert_equal "1" [run_cli incr counter]
    }

    test_nontty_cli "Bulk reply" {
        r set key "tab\tnewline\n"
        assert_equal "tab\tnewline" [run_cli get key]
    }

    test_nontty_cli "Multi-bulk reply" {
        r del list
        r rpush list foo
        r rpush list bar
        assert_equal "foo\nbar" [run_cli lrange list 0 -1]
    }

    test_nontty_cli "Quoted input arguments" {
        r set "\x00\x00" "value"
        assert_equal "value" [run_cli --quoted-input get {"\x00\x00"}]
    }

    test_nontty_cli "No accidental unquoting of input arguments" {
        run_cli --quoted-input set {"\x41\x41"} quoted-val
        run_cli set {"\x41\x41"} unquoted-val

        assert_equal "quoted-val" [r get AA]
        assert_equal "unquoted-val" [r get {"\x41\x41"}]
    }

    test_nontty_cli "Invalid quoted input arguments" {
        catch {run_cli --quoted-input set {"Unterminated}} err
        assert_match {*exited abnormally*} $err

        # A single arg that unquotes to two arguments is also not expected
        catch {run_cli --quoted-input set {"arg1" "arg2"}} err
        assert_match {*exited abnormally*} $err
    }

    test_nontty_cli "Read last argument from pipe" {
        assert_equal "OK" [run_cli_with_input_pipe "echo foo" set key]
        assert_equal "foo\n" [r get key]
    }

    test_nontty_cli "Read last argument from file" {
        set tmpfile [write_tmpfile "from file"]
        assert_equal "OK" [run_cli_with_input_file $tmpfile set key]
        assert_equal "from file" [r get key]
        file delete $tmpfile
    }

    proc test_redis_cli_rdb_dump {} {
        r flushdb

        set dir [lindex [r config get dir] 1]

        assert_equal "OK" [r debug populate 100000 key 1000]
        catch {run_cli --rdb "$dir/cli.rdb"} output
        assert_match {*Transfer finished with success*} $output

        file delete "$dir/dump.rdb"
        file rename "$dir/cli.rdb" "$dir/dump.rdb"

        assert_equal "OK" [r set should-not-exist 1]
        assert_equal "OK" [r debug reload nosave]
        assert_equal {} [r get should-not-exist]
    }

    test "Dumping an RDB" {
        # Disk-based master
        assert_match "OK" [r config set repl-diskless-sync no]
        test_redis_cli_rdb_dump

        # Disk-less master
        assert_match "OK" [r config set repl-diskless-sync yes]
        assert_match "OK" [r config set repl-diskless-sync-delay 0]
        test_redis_cli_rdb_dump
    }

    test "Scan mode" {
        r flushdb
        populate 1000 key: 1

        # basic use
        assert_equal 1000 [llength [split [run_cli --scan]]]

        # pattern
        assert_equal {key:2} [run_cli --scan --pattern "*:2"]

        # pattern matching with a quoted string
        assert_equal {key:2} [run_cli --scan --quoted-pattern {"*:\x32"}]
    }

    test "Connecting as a replica" {
        set fd [open_cli "--replica"]
        wait_for_condition 500 500 {
            [string match {*slave0:*state=online*} [r info]]
        } else {
            fail "redis-cli --replica did not connect"
        }

        for {set i 0} {$i < 100} {incr i} {
           r set test-key test-value-$i
        }
        r client kill type slave
        catch {
            assert_match {*SET*key-a*} [read_cli $fd]
        }

        close_cli $fd
    }

    test "Piping raw protocol" {
        set cmds [tmpfile "cli_cmds"]
        set cmds_fd [open $cmds "w"]

        puts $cmds_fd [formatCommand select 9]
        puts $cmds_fd [formatCommand del test-counter]

        for {set i 0} {$i < 1000} {incr i} {
            puts $cmds_fd [formatCommand incr test-counter]
            puts $cmds_fd [formatCommand set large-key [string repeat "x" 20000]]
        }

        for {set i 0} {$i < 100} {incr i} {
            puts $cmds_fd [formatCommand set very-large-key [string repeat "x" 512000]]
        }
        close $cmds_fd

        set cli_fd [open_cli "--pipe" $cmds]
        fconfigure $cli_fd -blocking true
        set output [read_cli $cli_fd]

        assert_equal {1000} [r get test-counter]
        assert_match {*All data transferred*errors: 0*replies: 2102*} $output

        file delete $cmds
    }
}
