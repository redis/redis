tags {"external:skip"} {

set system_name [string tolower [exec uname -s]]
set backtrace_supported [system_backtrace_supported]
set threads_mngr_supported 0 ;# Do we support printing stack trace from all threads, not just the one that got the signal?
if {$system_name eq {linux}} {
    set threads_mngr_supported 1
}

# look for the DEBUG command in the backtrace, used when we triggered
# a stack trace print while we know redis is running that command.
proc check_log_backtrace_for_debug {log_pattern} {
    # search for the final line in the stacktraces generation to make sure it was completed.
    set pattern "* STACK TRACE DONE *"
    set res [wait_for_log_messages 0 \"$pattern\" 0 100 100]

    set res [wait_for_log_messages 0 \"$log_pattern\" 0 100 100]
    if {$::verbose} { puts $res}

    # If the stacktrace is printed more than once, it means redis crashed during crash report generation
    assert_equal [count_log_message 0 "STACK TRACE -"] 1

    upvar threads_mngr_supported threads_mngr_supported

    # the following checks are only done if we support printing stack trace from all threads
    if {$threads_mngr_supported} {
        assert_equal [count_log_message 0 "setupStacktracePipe failed"] 0
        assert_equal [count_log_message 0 "failed to open /proc/"] 0
        assert_equal [count_log_message 0 "failed to find SigBlk or/and SigIgn"] 0
        # the following are skipped since valgrind is slow and a timeout can happen
        if {!$::valgrind} {
            assert_equal [count_log_message 0 "wait_threads(): waiting threads timed out"] 0
            # make sure redis prints stack trace for all threads. we know 3 threads are idle in bio.c
            assert_equal [count_log_message 0 "bioProcessBackgroundJobs"] 3
        }
    }

    set pattern "*debugCommand*"
    set res [wait_for_log_messages 0 \"$pattern\" 0 100 100]
    if {$::verbose} { puts $res}
}

# used when backtrace_supported == 0
proc check_crash_log {log_pattern} {
    set res [wait_for_log_messages 0 \"$log_pattern\" 0 50 100]
    if {$::verbose} { puts $res }
}

# test the watchdog and the stack trace report from multiple threads
if {$backtrace_supported} {
    set server_path [tmpdir server.log]
    start_server [list overrides [list dir $server_path]] {
        test "Server is able to generate a stack trace on selected systems" {
            r config set watchdog-period 200
            r debug sleep 1
            
            check_log_backtrace_for_debug "*WATCHDOG TIMER EXPIRED*"
            # make sure redis is still alive
            assert_equal "PONG" [r ping]
        }
    }
}

# Valgrind will complain that the process terminated by a signal, skip it.
if {!$::valgrind} {
    if {$backtrace_supported} {
        set check_cb check_log_backtrace_for_debug
    } else {  
        set check_cb check_crash_log
    }

    # test being killed by a SIGABRT from outside
    set server_path [tmpdir server1.log]
    start_server [list overrides [list dir $server_path crash-memcheck-enabled no]] {
        test "Crash report generated on SIGABRT" {
            set pid [s process_id]
            r deferred 1
            r debug sleep 10 ;# so that we see the function in the stack trace
            r flush
            after 100 ;# wait for redis to get into the sleep
            exec kill -SIGABRT $pid
            $check_cb "*crashed by signal*"
        }
    }

    # test DEBUG SEGFAULT
    set server_path [tmpdir server2.log]
    start_server [list overrides [list dir $server_path crash-memcheck-enabled no]] {
        test "Crash report generated on DEBUG SEGFAULT" {
            catch {r debug segfault}
            $check_cb "*crashed by signal*"
        }
    }

    # test DEBUG SIGALRM being non-fatal
    set server_path [tmpdir server3.log]
    start_server [list overrides [list dir $server_path]] {
        test "Stacktraces generated on SIGALRM" {
            set pid [s process_id]
            r deferred 1
            r debug sleep 10 ;# so that we see the function in the stack trace
            r flush
            after 100 ;# wait for redis to get into the sleep
            exec kill -SIGALRM $pid
            $check_cb "*Received SIGALRM*"
            r read
            r deferred 0
            # make sure redis is still alive
            assert_equal "PONG" [r ping]
        }
    }
}

# test DEBUG ASSERT
if {$backtrace_supported} {
    set server_path [tmpdir server4.log]
    # Use exit() instead of abort() upon assertion so Valgrind tests won't fail.
    start_server [list overrides [list dir $server_path use-exit-on-panic yes crash-memcheck-enabled no]] {
        test "Generate stacktrace on assertion" {
            catch {r debug assert}
            check_log_backtrace_for_debug "*ASSERTION FAILED*"
        }
    }
}

}
