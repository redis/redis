tags {"external:skip"} {

set system_name [string tolower [exec uname -s]]
set backtrace_supported 0

# We only support darwin or Linux with glibc
if {$system_name eq {darwin}} {
    set backtrace_supported 1
} elseif {$system_name eq {linux}} {
    # Avoid the test on libmusl, which does not support backtrace
    set ldd [exec ldd src/redis-server]
    if {![string match {*libc.musl*} $ldd]} {
        set backtrace_supported 1
    }
}

if {$backtrace_supported} {
    set server_path [tmpdir server.log]
    start_server [list overrides [list dir $server_path]] {
        test "Server is able to generate a stack trace on selected systems" {
            r config set watchdog-period 200
            r debug sleep 1
            set pattern "*debugCommand*"
            set res [wait_for_log_messages 0 \"$pattern\" 0 100 100]
            if {$::verbose} { puts $res }
        }
    }
}

# Valgrind will complain that the process terminated by a signal, skip it.
if {!$::valgrind} {
    if {$backtrace_supported} {
        set crash_pattern "*STACK TRACE*"
    } else {
        set crash_pattern "*crashed by signal*"
    }

    set server_path [tmpdir server1.log]
    start_server [list overrides [list dir $server_path]] {
        test "Crash report generated on SIGABRT" {
            set pid [s process_id]
            exec kill -SIGABRT $pid
            set res [wait_for_log_messages 0 \"$crash_pattern\" 0 50 100]
            if {$::verbose} { puts $res }
        }
    }

    set server_path [tmpdir server2.log]
    start_server [list overrides [list dir $server_path]] {
        test "Crash report generated on DEBUG SEGFAULT" {
            catch {r debug segfault}
            set res [wait_for_log_messages 0 \"$crash_pattern\" 0 50 100]
            if {$::verbose} { puts $res }
        }
    }
}

}
