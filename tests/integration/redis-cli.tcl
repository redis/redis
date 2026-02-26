source tests/support/cli.tcl

if {$::singledb} {
    set ::dbnum 0
} else {
    set ::dbnum 9
}

file delete ./.rediscli_history_test
set ::env(REDISCLI_HISTFILE) ".rediscli_history_test"

start_server {tags {"cli"}} {
    proc open_cli {{opts ""} {infile ""}} {
        if { $opts == "" } {
            set opts "-n $::dbnum"
        }
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
        set ret [read $fd]
        while {[string length $ret] == 0} {
            after 10
            set ret [read $fd]
        }

        # We may have a short read, try to read some more.
        set empty_reads 0
        while {$empty_reads < 5} {
            set buf [read $fd]
            if {[string length $buf] == 0} {
                after 10
                incr empty_reads
            } else {
                append ret $buf
                set empty_reads 0
            }
        }
        return $ret
    }

    proc write_cli {fd buf} {
        puts $fd $buf
        flush $fd
    }

    # Helpers to run tests in interactive mode

    proc format_output {output} {
        set _ [string trimright $output "\n"]
    }

    proc run_command {fd cmd} {
        write_cli $fd $cmd
        set _ [format_output [read_cli $fd]]
    }

    proc test_interactive_cli_with_prompt {name code} {
        set ::env(FAKETTY_WITH_PROMPT) 1
        test_interactive_cli $name $code
        unset ::env(FAKETTY_WITH_PROMPT)
    }

    proc test_interactive_cli {name code} {
        set ::env(FAKETTY) 1
        set fd [open_cli]
        test "Interactive CLI: $name" $code
        close_cli $fd
        unset ::env(FAKETTY)
    }

    proc test_interactive_nontty_cli {name code} {
        set fd [open_cli]
        test "Interactive non-TTY CLI: $name" $code
        close_cli $fd
    }

    # Helpers to run tests where stdout is not a tty
    proc write_tmpfile {contents} {
        set tmp [tmpfile "cli"]
        set tmpfd [open $tmp "w"]
        puts -nonewline $tmpfd $contents
        close $tmpfd
        set _ $tmp
    }

    proc _run_cli {host port db opts args} {
        set cmd [rediscli $host $port [list -n $db {*}$args]]
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
        _run_cli [srv host] [srv port] $::dbnum {} {*}$args
    }

    proc run_cli_with_input_pipe {mode cmd args} {
        if {$mode == "x" } {
            _run_cli [srv host] [srv port] $::dbnum [list pipe $cmd] -x {*}$args
        } elseif {$mode == "X"} {
            _run_cli [srv host] [srv port] $::dbnum [list pipe $cmd] -X tag {*}$args
        }
    }

    proc run_cli_with_input_file {mode path args} {
        if {$mode == "x" } {
            _run_cli [srv host] [srv port] $::dbnum [list path $path] -x {*}$args
        } elseif {$mode == "X"} {
            _run_cli [srv host] [srv port] $::dbnum [list path $path] -X tag {*}$args
        }
    }

    proc run_cli_host_port_db {host port db args} {
        _run_cli $host $port $db {} {*}$args
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

    test_interactive_cli_with_prompt "should find first search result" {
        run_command $fd "keys one\x0D"
        run_command $fd "keys two\x0D"

        puts $fd "\x12" ;# CTRL+R
        read_cli $fd

        puts -nonewline $fd "ey"
        set result [read_cli $fd]
        assert_equal 1 [regexp {\(reverse-i-search\): \x1B\[0mk\x1B\[1mey\x1B\[0ms two} $result]
    }

    test_interactive_cli_with_prompt "should find and use the first search result" {
        set now [clock seconds]
        run_command $fd "SET blah \"myvalue\"\x0D"
        run_command $fd "GET blah\x0D"

        puts $fd "\x12" ;# CTRL+R
        read_cli $fd

        puts -nonewline $fd "ET b"
        set result [read_cli $fd]
        assert_equal 1 [regexp {\(reverse-i-search\): \x1B\[0mG\x1B\[1mET b\x1B\[0mlah} $result]

        puts $fd "\x0D" ;# ENTER
        set result2 [read_cli $fd]
        assert_equal 1 [regexp {.*"myvalue"\n} $result2]
    }

    test_interactive_cli_with_prompt "should be ok if there is no result" {
        puts $fd "\x12" ;# CTRL+R

        set now [clock seconds]
        puts $fd "\x12" ;# CTRL+R
        set result [read_cli $fd]
        assert_equal 1 [regexp {\(reverse-i-search\):} $result]

        set result2 [run_command $fd "keys \"$now\"\x0D"]
        assert_equal 1 [regexp {.*(empty array).*} $result2]
    }

    test_interactive_cli_with_prompt "upon submitting search, (reverse-i-search) prompt should go away" {
        puts $fd "\x12" ;# CTRL+R

        set now [clock seconds]
        set result [read_cli $fd]
        assert_equal 1 [regexp {\(reverse-i-search\):} $result]

        set result2 [run_command $fd "keys \"$now\"\x0D"]

        assert_equal 1 [regexp {127\.0\.0\.1:[0-9]*(\[[0-9]])?>} $result2]
    }

    test_interactive_cli_with_prompt "should find second search result if user presses ctrl+r again" {
        run_command $fd "keys one\x0D"
        run_command $fd "keys two\x0D"

        puts $fd "\x12" ;# CTRL+R
        read_cli $fd

        puts -nonewline $fd "ey"
        set result [read_cli $fd]
        assert_equal 1 [regexp {\(reverse-i-search\): \x1B\[0mk\x1B\[1mey\x1B\[0ms two} $result]

        puts $fd "\x12" ;# CTRL+R
        set result [read_cli $fd]
        assert_equal 1 [regexp {\(reverse-i-search\): \x1B\[0mk\x1B\[1mey\x1B\[0ms one} $result]
    }

    test_interactive_cli_with_prompt "should find second search result if user presses ctrl+s" {
        run_command $fd "keys one\x0D"
        run_command $fd "keys two\x0D"

        puts $fd "\x13" ;# CTRL+S
        read_cli $fd

        puts -nonewline $fd "ey"
        set result [read_cli $fd]
        assert_equal 1 [regexp {\(i-search\): \x1B\[0mk\x1B\[1mey\x1B\[0ms one} $result]

        puts $fd "\x13" ;# CTRL+S
        set result [read_cli $fd]
        assert_equal 1 [regexp {\(i-search\): \x1B\[0mk\x1B\[1mey\x1B\[0ms two} $result]
    }

    test_interactive_cli_with_prompt "should exit reverse search if user presses ctrl+g" {
        run_command $fd ""

        puts $fd "\x12" ;# CTRL+R
        set result [read_cli $fd]
        assert_equal 1 [regexp {\(reverse-i-search\):} $result]

        puts $fd "\x07" ;# CTRL+G
        set result2 [read_cli $fd]
        assert_equal 1 [regexp {127\.0\.0\.1:[0-9]*(\[[0-9]])?>} $result2]
    }

    test_interactive_cli_with_prompt "should exit reverse search if user presses up arrow" {
        run_command $fd ""

        puts $fd "\x12" ;# CTRL+R
        set result [read_cli $fd]
        assert_equal 1 [regexp {\(reverse-i-search\):} $result]

        puts $fd "\x1B\x5B\x41" ;# up arrow
        set result2 [read_cli $fd]
        assert_equal 1 [regexp {127\.0\.0\.1:[0-9]*(\[[0-9]])?>} $result2]
    }

    test_interactive_cli_with_prompt "should exit reverse search if user presses right arrow" {
        run_command $fd ""

        puts $fd "\x12" ;# CTRL+R
        set result [read_cli $fd]
        assert_equal 1 [regexp {\(reverse-i-search\):} $result]

        puts $fd "\x1B\x5B\x42" ;# right arrow
        set result2 [read_cli $fd]
        assert_equal 1 [regexp {127\.0\.0\.1:[0-9]*(\[[0-9]])?>} $result2]
    }

    test_interactive_cli_with_prompt "should exit reverse search if user presses down arrow" {
        run_command $fd ""

        puts $fd "\x12" ;# CTRL+R
        set result [read_cli $fd]
        assert_equal 1 [regexp {\(reverse-i-search\):} $result]

        puts $fd "\x1B\x5B\x43" ;# down arrow
        set result2 [read_cli $fd]
        assert_equal 1 [regexp {127\.0\.0\.1:[0-9]*(\[[0-9]])?>} $result2]
    }

    test_interactive_cli_with_prompt "should exit reverse search if user presses left arrow" {
        run_command $fd ""

        puts $fd "\x12" ;# CTRL+R
        set result [read_cli $fd]
        assert_equal 1 [regexp {\(reverse-i-search\):} $result]

        puts $fd "\x1B\x5B\x44" ;# left arrow
        set result2 [read_cli $fd]
        assert_equal 1 [regexp {127\.0\.0\.1:[0-9]*(\[[0-9]])?>} $result2]
    }

    test_interactive_cli_with_prompt "should disable and persist line if user presses tab" {
        run_command $fd ""

        puts $fd "\x12" ;# CTRL+R
        set result [read_cli $fd]
        assert_equal 1 [regexp {\(reverse-i-search\):} $result]

        puts -nonewline $fd "GET blah"
        read_cli $fd

        puts -nonewline $fd "\x09" ;# TAB
        set result2 [read_cli $fd]
        assert_equal 1 [regexp {127\.0\.0\.1:[0-9]*(\[[0-9]])?> GET blah} $result2]
    }

    test_interactive_cli_with_prompt "should disable and persist search result if user presses tab" {
        run_command $fd "GET one\x0D"

        puts $fd "\x12" ;# CTRL+R
        set result [read_cli $fd]
        assert_equal 1 [regexp {\(reverse-i-search\):} $result]

        puts -nonewline $fd "one"
        read_cli $fd

        puts -nonewline $fd "\x09" ;# TAB
        set result2 [read_cli $fd]
        assert_equal 1 [regexp {127\.0\.0\.1:[0-9]*(\[[0-9]])?> GET one} $result2]
    }

    test_interactive_cli_with_prompt "should disable and persist line and move the cursor if user presses tab" {
        run_command $fd ""

        puts $fd "\x12" ;# CTRL+R
        set result [read_cli $fd]
        assert_equal 1 [regexp {\(reverse-i-search\):} $result]

        puts -nonewline $fd "GET blah"
        read_cli $fd

        puts -nonewline $fd "\x09" ;# TAB
        set result2 [read_cli $fd]
        assert_equal 1 [regexp {127\.0\.0\.1:[0-9]*(\[[0-9]])?> GET blah} $result2]

        puts -nonewline $fd "suffix"
        set result3 [read_cli $fd]
        assert_equal 1 [regexp {127\.0\.0\.1:[0-9]*(\[[0-9]])?> GET blahsuffix} $result3]
    }

    test_interactive_cli "INFO response should be printed raw" {
        set lines [split [run_command $fd info] "\n"]
        foreach line $lines {
            # Info lines end in \r\n, so they now end in \r.
            if {![regexp {^\r$|^#|^[^#:]+:} $line]} {
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

    test_interactive_cli "Subscribed mode" {
        if {$::force_resp3} {
            run_command $fd "hello 3"
        }

        set reading "Reading messages... (press Ctrl-C to quit or any key to type command)\r"
        set erase "\033\[K"; # Erases the "Reading messages..." line.

        # Subscribe to some channels.
        set sub1 "1) \"subscribe\"\n2) \"ch1\"\n3) (integer) 1\n"
        set sub2 "1) \"subscribe\"\n2) \"ch2\"\n3) (integer) 2\n"
        set sub3 "1) \"subscribe\"\n2) \"ch3\"\n3) (integer) 3\n"
        assert_equal $sub1$sub2$sub3$reading \
            [run_command $fd "subscribe ch1 ch2 ch3"]

        # Receive pubsub message.
        r publish ch2 hello
        set message "1) \"message\"\n2) \"ch2\"\n3) \"hello\"\n"
        assert_equal $erase$message$reading [read_cli $fd]

        # Unsubscribe some.
        set unsub1 "1) \"unsubscribe\"\n2) \"ch1\"\n3) (integer) 2\n"
        set unsub2 "1) \"unsubscribe\"\n2) \"ch2\"\n3) (integer) 1\n"
        assert_equal $erase$unsub1$unsub2$reading \
            [run_command $fd "unsubscribe ch1 ch2"]

        run_command $fd "hello 2"

        # Command forbidden in subscribed mode (RESP2).
        set err "(error) ERR Can't execute 'get': only (P|S)SUBSCRIBE / (P|S)UNSUBSCRIBE / PING / QUIT / RESET are allowed in this context\n"
        assert_equal $erase$err$reading [run_command $fd "get k"]

        # Command allowed in subscribed mode.
        set pong "1) \"pong\"\n2) \"\"\n"
        assert_equal $erase$pong$reading [run_command $fd "ping"]

        # Reset exits subscribed mode.
        assert_equal ${erase}RESET [run_command $fd "reset"]
        assert_equal PONG [run_command $fd "ping"]

        # Check TTY output of push messages in RESP3 has ")" prefix (to be changed to ">" in the future).
        assert_match "1#*" [run_command $fd "hello 3"]
        set sub1 "1) \"subscribe\"\n2) \"ch1\"\n3) (integer) 1\n"
        assert_equal $sub1$reading \
            [run_command $fd "subscribe ch1"]
    }

    test_interactive_nontty_cli "Subscribed mode" {
        # Raw output and no "Reading messages..." info message.
        # Use RESP3 in this test case.
        assert_match {*proto 3*} [run_command $fd "hello 3"]

        # Subscribe to some channels.
        set sub1 "subscribe\nch1\n1"
        set sub2 "subscribe\nch2\n2"
        assert_equal $sub1\n$sub2 \
            [run_command $fd "subscribe ch1 ch2"]

        assert_equal OK [run_command $fd "client tracking on"]
        assert_equal OK [run_command $fd "set k 42"]
        assert_equal 42 [run_command $fd "get k"]

        # Interleaving invalidate and pubsub messages.
        r publish ch1 hello
        r del k
        r publish ch2 world
        set message1 "message\nch1\nhello"
        set invalidate "invalidate\nk"
        set message2 "message\nch2\nworld"
        assert_equal $message1\n$invalidate\n$message2\n [read_cli $fd]

        # Unsubscribe all.
        set unsub1 "unsubscribe\nch1\n1"
        set unsub2 "unsubscribe\nch2\n0"
        assert_equal $unsub1\n$unsub2 [run_command $fd "unsubscribe ch1 ch2"]
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
        assert_equal "OK" [run_cli_with_input_pipe x "echo foo" set key]
        assert_equal "foo\n" [r get key]

        assert_equal "OK" [run_cli_with_input_pipe X "echo foo" set key2 tag]
        assert_equal "foo\n" [r get key2]
    }

    test_tty_cli "Read last argument from file" {
        set tmpfile [write_tmpfile "from file"]

        assert_equal "OK" [run_cli_with_input_file x $tmpfile set key]
        assert_equal "from file" [r get key]

        assert_equal "OK" [run_cli_with_input_file X $tmpfile set key2 tag]
        assert_equal "from file" [r get key2]

        file delete $tmpfile
    }

    test_tty_cli "Escape character in JSON mode" {
        # reverse solidus
        r hset solidus \/ \/
        assert_equal \/ \/ [run_cli hgetall solidus]
        set escaped_reverse_solidus \"\\"
        assert_equal $escaped_reverse_solidus $escaped_reverse_solidus [run_cli --json hgetall \/]
        # non printable (0xF0 in ISO-8859-1, not UTF-8(0xC3 0xB0))
        set eth "\u00f0\u0065"
        r hset eth test $eth
        assert_equal \"\\xf0e\" [run_cli hget eth test]
        assert_equal \"\u00f0e\" [run_cli --json hget eth test]
        assert_equal \"\\\\xf0e\" [run_cli --quoted-json hget eth test]
        # control characters
        r hset control test "Hello\x00\x01\x02\x03World"
        assert_equal \"Hello\\u0000\\u0001\\u0002\\u0003World" [run_cli --json hget control test]
        # non-string keys
        r hset numkey 1 One
        assert_equal \{\"1\":\"One\"\} [run_cli --json hgetall numkey]
        # non-string, non-printable keys
        r hset npkey "K\u0000\u0001ey" "V\u0000\u0001alue"
        assert_equal \{\"K\\u0000\\u0001ey\":\"V\\u0000\\u0001alue\"\} [run_cli --json hgetall npkey]
        assert_equal \{\"K\\\\x00\\\\x01ey\":\"V\\\\x00\\\\x01alue\"\} [run_cli --quoted-json hgetall npkey]
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

if {!$::tls} { ;# fake_redis_node doesn't support TLS
    test_nontty_cli "ASK redirect test" {
        # Set up two fake Redis nodes.
        set tclsh [info nameofexecutable]
        set script "tests/helpers/fake_redis_node.tcl"
        set port1 [find_available_port $::baseport $::portcount]
        set port2 [find_available_port $::baseport $::portcount]
        set p1 [exec $tclsh $script $port1 \
                "SET foo bar" "-ASK 12182 127.0.0.1:$port2" &]
        set p2 [exec $tclsh $script $port2 \
                "ASKING" "+OK" \
                "SET foo bar" "+OK" &]
        # Make sure both fake nodes have started listening
        wait_for_condition 50 50 {
            [catch {close [socket "127.0.0.1" $port1]}] == 0 && \
            [catch {close [socket "127.0.0.1" $port2]}] == 0
        } else {
            fail "Failed to start fake Redis nodes"
        }
        # Run the cli
        assert_equal "OK" [run_cli_host_port_db "127.0.0.1" $port1 0 -c SET foo bar]
    }
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
        assert_equal "OK" [run_cli_with_input_pipe x "echo foo" set key]
        assert_equal "foo\n" [r get key]

        assert_equal "OK" [run_cli_with_input_pipe X "echo foo" set key2 tag]
        assert_equal "foo\n" [r get key2]
    }

    test_nontty_cli "Read last argument from file" {
        set tmpfile [write_tmpfile "from file"]

        assert_equal "OK" [run_cli_with_input_file x $tmpfile set key]
        assert_equal "from file" [r get key]

        assert_equal "OK" [run_cli_with_input_file X $tmpfile set key2 tag]
        assert_equal "from file" [r get key2]

        file delete $tmpfile
    }

    test_nontty_cli "Test command-line hinting - latest server" {
        # cli will connect to the running server and will use COMMAND DOCS
        catch {run_cli --test_hint_file tests/assets/test_cli_hint_suite.txt} output
        assert_match "*SUCCESS*" $output
    }

    test_nontty_cli "Test command-line hinting - no server" {
        # cli will fail to connect to the server and will use the cached commands.c
        catch {run_cli -p 123 --test_hint_file tests/assets/test_cli_hint_suite.txt} output
        assert_match "*SUCCESS*" $output
    }

    test_nontty_cli "Test command-line hinting - old server" {
        # cli will connect to the server but will not use COMMAND DOCS,
        # and complete the missing info from the cached commands.c
        r ACL setuser clitest on nopass +@all -command|docs
        catch {run_cli --user clitest -a nopass --no-auth-warning --test_hint_file tests/assets/test_cli_hint_suite.txt} output
        assert_match "*SUCCESS*" $output
        r acl deluser clitest
    }
    
    proc test_redis_cli_rdb_dump {functions_only} {
        r flushdb
        r function flush

        if {!$::external} {
            set lines [count_log_lines 0]
        }
        set dir [lindex [r config get dir] 1]

        assert_equal "OK" [r debug populate 100000 key 1000]
        assert_equal "lib1" [r function load "#!lua name=lib1\nredis.register_function('func1', function() return 123 end)"]
        if {$functions_only} {
            set args "--functions-rdb $dir/cli.rdb"
        } else {
            set args "--rdb $dir/cli.rdb"
        }
        catch {run_cli {*}$args} output
        assert_match {*Transfer finished with success*} $output

        # If server enabled diskless sync, it should transfer the RDB by RDB channel
        # since server will automatically apply this optimization for rdb-only.
        if {[r config get repl-diskless-sync] == "yes" && !$::external} {
            verify_log_message 0 "*replicas sockets (rdb-channel)*" $lines
        }

        file delete "$dir/dump.rdb"
        file rename "$dir/cli.rdb" "$dir/dump.rdb"

        assert_equal "OK" [r set should-not-exist 1]
        assert_equal "should_not_exist_func" [r function load "#!lua name=should_not_exist_func\nredis.register_function('should_not_exist_func', function() return 456 end)"]
        assert_equal "OK" [r debug reload nosave]
        assert_equal {} [r get should-not-exist]
        assert_equal {{library_name lib1 engine LUA functions {{name func1 description {} flags {}}}}} [r function list]
        if {$functions_only} {
            assert_equal 0 [r dbsize]
        } else {
            assert_equal 100000 [r dbsize]
        }
    }

    foreach {functions_only} {no yes} {

    test "Dumping an RDB - functions only: $functions_only" {
        # Disk-based master
        assert_match "OK" [r config set repl-diskless-sync no]
        test_redis_cli_rdb_dump $functions_only

        # Disk-less master
        assert_match "OK" [r config set repl-diskless-sync yes]
        assert_match "OK" [r config set repl-diskless-sync-delay 0]
        test_redis_cli_rdb_dump $functions_only
    } {} {needs:repl needs:debug}

    } ;# foreach functions_only

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

    proc test_redis_cli_repl {} {
        set fd [open_cli "--replica"]
        wait_for_condition 500 100 {
            [string match {*slave0:*state=online*} [r info]]
        } else {
            fail "redis-cli --replica did not connect"
        }

        for {set i 0} {$i < 100} {incr i} {
           r set test-key test-value-$i
        }

        wait_for_condition 500 100 {
            [string match {*test-value-99*} [read_cli $fd]]
        } else {
            fail "redis-cli --replica didn't read commands"
        }

        fconfigure $fd -blocking true
        r client kill type slave
        catch { close_cli $fd } err
        assert_match {*Server closed the connection*} $err
    }

    test "Connecting as a replica" {
        # Disk-based master
        assert_match "OK" [r config set repl-diskless-sync no]
        test_redis_cli_repl

        # Disk-less master
        assert_match "OK" [r config set repl-diskless-sync yes]
        assert_match "OK" [r config set repl-diskless-sync-delay 0]
        test_redis_cli_repl
    } {} {needs:repl}

    test "Piping raw protocol" {
        set cmds [tmpfile "cli_cmds"]
        set cmds_fd [open $cmds "w"]

        set cmds_count 2101

        if {!$::singledb} {
            puts $cmds_fd [formatCommand select 9]
            incr cmds_count
        }
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
        assert_match "*All data transferred*errors: 0*replies: ${cmds_count}*" $output

        file delete $cmds
    }

    test "Options -X with illegal argument" {
        assert_error "*-x and -X are mutually exclusive*" {run_cli -x -X tag}

        assert_error "*Unrecognized option or bad number*" {run_cli -X}

        assert_error "*tag not match*" {run_cli_with_input_pipe X "echo foo" set key wrong_tag}
    }

    test "DUMP RESTORE with -x option" {
        set cmdline [rediscli [srv host] [srv port]]

        exec {*}$cmdline DEL set new_set
        exec {*}$cmdline SADD set 1 2 3 4 5 6
        assert_equal 6 [exec {*}$cmdline SCARD set]

        assert_equal "OK" [exec {*}$cmdline -D "" --raw DUMP set | \
                                {*}$cmdline -x RESTORE new_set 0]

        assert_equal 6 [exec {*}$cmdline SCARD new_set]
        assert_equal "1\n2\n3\n4\n5\n6" [exec {*}$cmdline SMEMBERS new_set]
    }

    test "DUMP RESTORE with -X option" {
        set cmdline [rediscli [srv host] [srv port]]

        exec {*}$cmdline DEL zset new_zset
        exec {*}$cmdline ZADD zset 1 a 2 b 3 c
        assert_equal 3 [exec {*}$cmdline ZCARD zset]

        assert_equal "OK" [exec {*}$cmdline -D "" --raw DUMP zset | \
                                {*}$cmdline -X dump_tag RESTORE new_zset 0 dump_tag REPLACE]

        assert_equal 3 [exec {*}$cmdline ZCARD new_zset]
        assert_equal "a\n1\nb\n2\nc\n3" [exec {*}$cmdline ZRANGE new_zset 0 -1 WITHSCORES]
    }

    test "Send eval command by using --eval option" {
        set tmpfile [write_tmpfile {return ARGV[1]}]
        set cmdline [rediscli [srv host] [srv port]]
        assert_equal "foo" [exec {*}$cmdline --eval $tmpfile , foo]
    }
}

start_server {tags {"cli external:skip"}} {
    test_interactive_cli_with_prompt "db_num showed in redis-cli after reconnected" {
        run_command $fd "select 0\x0D"
        run_command $fd "set a zoo-0\x0D"
        run_command $fd "select 6\x0D"
        run_command $fd "set a zoo-6\x0D"
        r save

        # kill server and restart
        exec kill [s process_id]
        wait_for_log_messages 0 {"*Redis is now ready to exit*"} 0 1000 10
        catch {[run_command $fd "ping\x0D"]} err
        restart_server 0 true false 0

        # redis-cli should show '[6]' after reconnected and return 'zoo-6'
        write_cli $fd "GET a\x0D"
        after 100
        set result [format_output [read_cli $fd]]
        set regex {not connected> GET a.*"zoo-6".*127\.0\.0\.1:[0-9]*\[6\]>}
        assert_equal 1 [regexp $regex $result]
    }
}

file delete ./.rediscli_history_test
