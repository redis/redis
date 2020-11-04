set system_name [string tolower [exec uname -s]]

if {$system_name eq {linux} || $system_name eq {darwin}} {
    set server_path [tmpdir server.log]
    start_server [list overrides [list dir $server_path]] {
        test "Server is able to generate a stack trace on selected systems" {
            r config set watchdog-period 200
            r debug sleep 1
            set pattern "*debugCommand*"
            set retry 10
            while {$retry} {
                set result [exec tail -100 < [srv 0 stdout]]
                if {[string match $pattern $result]} {
                    break
                }
                incr retry -1
                after 1000
            }
            if {$retry == 0} {
                error "assertion:expected stack trace not found into log file"
            }
        }
    }

    # Valgrind will complain that the process terminated by a signal, skip it.
    if {!$::valgrind} {
        set server_path [tmpdir server1.log]
        start_server [list overrides [list dir $server_path]] {
            test "Crash report generated on SIGABRT" {
                set pid [s process_id]
                exec kill -SIGABRT $pid
                set pattern "*STACK TRACE*"
                set result [exec tail -1000 < [srv 0 stdout]]
                assert {[string match $pattern $result]}
            }
        }
    }

}
