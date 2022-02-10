# tests of corrupt ziplist payload with valid CRC

tags {"dump" "corruption" "external:skip"} {

# catch sigterm so that in case one of the random command hangs the test,
# usually due to redis not putting a response in the output buffers,
# we'll know which command it was
if { ! [ catch {
    package require Tclx
} err ] } {
    signal error SIGTERM
}

proc generate_collections {suffix elements} {
    set rd [redis_deferring_client]
    for {set j 0} {$j < $elements} {incr j} {
        # add both string values and integers
        if {$j % 2 == 0} {set val $j} else {set val "_$j"}
        $rd hset hash$suffix $j $val
        $rd lpush list$suffix $val
        $rd zadd zset$suffix $j $val
        $rd sadd set$suffix $val
        $rd xadd stream$suffix * item 1 value $val
    }
    for {set j 0} {$j < $elements * 5} {incr j} {
        $rd read ; # Discard replies
    }
    $rd close
}

# generate keys with various types and encodings
proc generate_types {} {
    r config set list-max-ziplist-size 5
    r config set hash-max-ziplist-entries 5
    r config set zset-max-ziplist-entries 5
    r config set stream-node-max-entries 5

    # create small (ziplist / listpack encoded) objects with 3 items
    generate_collections "" 3

    # add some metadata to the stream
    r xgroup create stream mygroup 0
    set records [r xreadgroup GROUP mygroup Alice COUNT 2 STREAMS stream >]
    r xdel stream [lindex [lindex [lindex [lindex $records 0] 1] 1] 0]
    r xack stream mygroup [lindex [lindex [lindex [lindex $records 0] 1] 0] 0]

    # create other non-collection types
    r incr int
    r set string str

    # create bigger objects with 10 items (more than a single ziplist / listpack)
    generate_collections big 10

    # make sure our big stream also has a listpack record that has different
    # field names than the master recorded
    r xadd streambig * item 1 value 1
    r xadd streambig * item 1 unique value
}

proc corrupt_payload {payload} {
    set len [string length $payload]
    set count 1 ;# usually corrupt only one byte
    if {rand() > 0.9} { set count 2 }
    while { $count > 0 } {
        set idx [expr {int(rand() * $len)}]
        set ch [binary format c [expr {int(rand()*255)}]]
        set payload [string replace $payload $idx $idx $ch]
        incr count -1
    }
    return $payload
}

# fuzzy tester for corrupt RESTORE payloads
# valgrind will make sure there were no leaks in the rdb loader error handling code
foreach sanitize_dump {no yes} {
    if {$::accurate} {
        set min_duration [expr {60 * 10}] ;# run at least 10 minutes
        set min_cycles 1000 ;# run at least 1k cycles (max 16 minutes)
    } else {
        set min_duration 10 ; # run at least 10 seconds
        set min_cycles 10 ; # run at least 10 cycles
    }

    # Don't execute this on FreeBSD due to a yet-undiscovered memory issue
    # which causes tclsh to bloat.
    if {[exec uname] == "FreeBSD"} {
        set min_cycles 1
        set min_duration 1
    }

    test "Fuzzer corrupt restore payloads - sanitize_dump: $sanitize_dump" {
        if {$min_duration * 2 > $::timeout} {
            fail "insufficient timeout"
        }
        # start a server, fill with data and save an RDB file once (avoid re-save)
        start_server [list overrides [list "save" "" use-exit-on-panic yes crash-memcheck-enabled no loglevel verbose] ] {
            set stdout [srv 0 stdout]
            r config set sanitize-dump-payload $sanitize_dump
            r debug set-skip-checksum-validation 1
            set start_time [clock seconds]
            generate_types
            set dbsize [r dbsize]
            r save
            set cycle 0
            set stat_terminated_in_restore 0
            set stat_terminated_in_traffic 0
            set stat_terminated_by_signal 0
            set stat_successful_restore 0
            set stat_rejected_restore 0
            set stat_traffic_commands_sent 0
            # repeatedly DUMP a random key, corrupt it and try RESTORE into a new key
            while true {
                set k [r randomkey]
                set dump [r dump $k]
                set dump [corrupt_payload $dump]
                set printable_dump [string2printable $dump]
                set restore_failed false
                set report_and_restart false
                set sent {}
                # RESTORE can fail, but hopefully not terminate
                if { [catch { r restore "_$k" 0 $dump REPLACE } err] } {
                    set restore_failed true
                    # skip if return failed with an error response.
                    if {[string match "ERR*" $err]} {
                        incr stat_rejected_restore
                    } else {
                        set report_and_restart true
                        incr stat_terminated_in_restore
                        write_log_line 0 "corrupt payload: $printable_dump"
                        if {$sanitize_dump == yes} {
                            puts "Server crashed in RESTORE with payload: $printable_dump"
                        }
                    }
                } else {
                    r ping ;# an attempt to check if the server didn't terminate (this will throw an error that will terminate the tests)
                }

                set print_commands false
                if {!$restore_failed} {
                    # if RESTORE didn't fail or terminate, run some random traffic on the new key
                    incr stat_successful_restore
                    if { [ catch {
                        set sent [generate_fuzzy_traffic_on_key "_$k" 1] ;# traffic for 1 second
                        incr stat_traffic_commands_sent [llength $sent]
                        r del "_$k" ;# in case the server terminated, here's where we'll detect it.
                        if {$dbsize != [r dbsize]} {
                            puts "unexpected keys"
                            puts "keys: [r keys *]"
                            puts $sent
                            exit 1
                        }
                    } err ] } {
                        # if the server terminated update stats and restart it
                        set report_and_restart true
                        incr stat_terminated_in_traffic
                        set by_signal [count_log_message 0 "crashed by signal"]
                        incr stat_terminated_by_signal $by_signal

                        if {$by_signal != 0 || $sanitize_dump == yes} {
                            puts "Server crashed (by signal: $by_signal), with payload: $printable_dump"
                            set print_commands true
                        }
                    }
                }

                # check valgrind report for invalid reads after each RESTORE
                # payload so that we have a report that is easier to reproduce
                set valgrind_errors [find_valgrind_errors [srv 0 stderr] false]
                set asan_errors [sanitizer_errors_from_file [srv 0 stderr]]
                if {$valgrind_errors != "" || $asan_errors != ""} {
                    puts "valgrind or asan found an issue for payload: $printable_dump"
                    set report_and_restart true
                    set print_commands true
                }

                if {$report_and_restart} {
                    if {$print_commands} {
                        puts "violating commands:"
                        foreach cmd $sent {
                            foreach arg $cmd {
                                puts -nonewline "[string2printable $arg] "
                            }
                            puts ""
                        }
                    }

                    # restart the server and re-apply debug configuration
                    write_log_line 0 "corrupt payload: $printable_dump"
                    restart_server 0 true true
                    r config set sanitize-dump-payload $sanitize_dump
                    r debug set-skip-checksum-validation 1
                }

                incr cycle
                if { ([clock seconds]-$start_time) >= $min_duration && $cycle >= $min_cycles} {
                    break
                }
            }
            if {$::verbose} {
                puts "Done $cycle cycles in [expr {[clock seconds]-$start_time}] seconds."
                puts "RESTORE: successful: $stat_successful_restore, rejected: $stat_rejected_restore"
                puts "Total commands sent in traffic: $stat_traffic_commands_sent, crashes during traffic: $stat_terminated_in_traffic ($stat_terminated_by_signal by signal)."
            }
        }
        # if we run sanitization we never expect the server to crash at runtime
        if {$sanitize_dump == yes} {
            assert_equal $stat_terminated_in_restore 0
            assert_equal $stat_terminated_in_traffic 0
        }
        # make sure all terminations where due to assertion and not a SIGSEGV
        assert_equal $stat_terminated_by_signal 0
    }
}



} ;# tags

