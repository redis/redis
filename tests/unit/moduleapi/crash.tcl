# This file is used to test certain crash edge cases to make sure they produce
# correct stack traces for debugging.
set testmodule [file normalize tests/modules/crash.so]
set backtrace_supported [system_backtrace_supported]

# Valgrind will complain that the process terminated by a signal, skip it.
if {!$::valgrind} {
    start_server {tags {"modules"}} {
        r module load $testmodule assert
        test {Test module crash when info crashes with an assertion } {
            catch {r 0 info infocrash}
            set res [wait_for_log_messages 0 {"*=== REDIS BUG REPORT START: Cut & paste starting from here ===*"} 0 10 1000]
            set loglines [lindex $res 1]

            set res [wait_for_log_messages 0 {"*ASSERTION FAILED*"} $loglines 10 1000]
            set loglines [lindex $res 1]

            set res [wait_for_log_messages 0 {"*RECURSIVE ASSERTION FAILED*"} $loglines 10 1000]
            set loglines [lindex $res 1]

            wait_for_log_messages 0 {"*=== REDIS BUG REPORT END. Make sure to include from START to END. ===*"} $loglines 10 1000
            assert_equal 1 [count_log_message 0 "=== REDIS BUG REPORT END. Make sure to include from START to END. ==="]
            assert_equal 2 [count_log_message 0 "ASSERTION FAILED"]
            if {$backtrace_supported} {
                # Make sure the crash trace is printed twice. There will be 3 instances of,
                # assertCrash 1 in the first stack trace and 2 in the second.
                assert_equal 3 [count_log_message 0 "assertCrash"]
            }
            assert_equal 1 [count_log_message 0 "RECURSIVE ASSERTION FAILED"]
            assert_equal 1 [count_log_message 0 "=== REDIS BUG REPORT START: Cut & paste starting from here ==="]
        }
    }

    start_server {tags {"modules"}} {
        r module load $testmodule segfault
        test {Test module crash when info crashes with a segfault} {
            catch {r 0 info infocrash}
            set res [wait_for_log_messages 0 {"*=== REDIS BUG REPORT START: Cut & paste starting from here ===*"} 0 10 1000]
            set loglines [lindex $res 1]

            if {$backtrace_supported} {
                set res [wait_for_log_messages 0 {"*Crashed running the instruction at*"} $loglines 10 1000]
                set loglines [lindex $res 1]

                set res [wait_for_log_messages 0 {"*Crashed running signal handler. Providing reduced version of recursive crash report*"} $loglines 10 1000]
                set loglines [lindex $res 1]
                set res [wait_for_log_messages 0 {"*Crashed running the instruction at*"} $loglines 10 1000]
                set loglines [lindex $res 1]
            }

            wait_for_log_messages 0 {"*=== REDIS BUG REPORT END. Make sure to include from START to END. ===*"} $loglines 10 1000
            assert_equal 1 [count_log_message 0 "=== REDIS BUG REPORT END. Make sure to include from START to END. ==="]
            assert_equal 1 [count_log_message 0 "Crashed running signal handler. Providing reduced version of recursive crash report"]
            if {$backtrace_supported} {
                assert_equal 2 [count_log_message 0 "Crashed running the instruction at"]
                # Make sure the crash trace is printed twice. There will be 3 instances of 
                # modulesCollectInfo, 1 in the first stack trace and 2 in the second.
                assert_equal 3 [count_log_message 0 "modulesCollectInfo"]
            }
            assert_equal 1 [count_log_message 0 "=== REDIS BUG REPORT START: Cut & paste starting from here ==="]
        }
    }
}
