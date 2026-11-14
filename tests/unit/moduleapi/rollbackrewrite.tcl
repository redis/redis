set testmodule [file normalize tests/modules/rollbackrewrite.so]

test {CONFIG rollback restores save persistence before module apply callbacks} {
    set include_file [file normalize [tmpfile rollback-save-include.conf]]
    set fd [open $include_file w]
    puts $fd {save 100 1}
    close $fd
    set config_file [file normalize [tmpfile rollback-save-main.conf]]
    set fd [open $config_file w]
    puts $fd "loadmodule \"$testmodule\""
    puts $fd "include \"$include_file\""
    puts $fd {save 200 2}
    close $fd
    set port [find_available_port $::baseport $::portcount]
    set dir [file normalize [tmpdir rollback-save]]
    set log_file [file normalize [tmpfile rollback-save-server.log]]
    set client ""
    set test_failed [catch {
        # Replay the CLI rule: restart_server does not retain launch arguments.
        for {set cycle 0} {$cycle < 2} {incr cycle} {
            exec src/redis-server $config_file --port $port --dir $dir --daemonize yes --logfile $log_file --save "250 5"
            wait_for_condition 100 50 {
                [count_message_lines $log_file "Ready to accept"] > $cycle
            } else {
                fail "Server did not start; log: $log_file"
            }
            set client [redis 127.0.0.1 $port 0 0]
            if {$cycle == 0} {
                assert_equal {100 1 200 2 250 5} [lindex [$client config get save] 1]
                assert_error {*timeout*} {
                    $client config set save "300 3" rollbackrewrite.enabled yes timeout -1
                }
                assert_equal {100 1 200 2 250 5} [lindex [$client config get save] 1]
                # Inspect the callback's rewrite, without another rewrite masking it.
                set saved_lines {}
                foreach line [split [read_file $config_file] "\n"] {
                    if {[string match {save *} $line]} {lappend saved_lines $line}
                }
            } else {
                # The clear and file rules also prove the callback rewrote the file.
                assert_equal [list {100 1 200 2 250 5} [list {save ""} {save 100 1} {save 200 2}]] \
                             [list [lindex [$client config get save] 1] $saved_lines]
            }
            catch {$client shutdown nosave}
            $client close
            set client ""
            wait_for_condition 100 50 {
                [count_message_lines $log_file "Redis is now ready to exit"] > $cycle
            } else {
                fail "Server did not stop; log: $log_file"
            }
        }
    } test_error]
    catch {$client shutdown nosave}
    catch {$client close}
    if {$test_failed} {error $test_error}
} {} {modules external:skip}
