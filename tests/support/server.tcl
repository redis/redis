set ::global_overrides {}
set ::tags {}
set ::valgrind_errors {}

proc start_server_error {config_file error} {
    set err {}
    append err "Can't start the Redis server\n"
    append err "CONFIGURATION:"
    append err [exec cat $config_file]
    append err "\nERROR:"
    append err [string trim $error]
    send_data_packet $::test_server_fd err $err
}

proc check_valgrind_errors stderr {
    set res [find_valgrind_errors $stderr true]
    if {$res != ""} {
        send_data_packet $::test_server_fd err "Valgrind error: $res\n"
    }
}

proc clean_persistence config {
    # we may wanna keep the logs for later, but let's clean the persistence
    # files right away, since they can accumulate and take up a lot of space
    set config [dict get $config "config"]
    set rdb [format "%s/%s" [dict get $config "dir"] "dump.rdb"]
    set aof [format "%s/%s" [dict get $config "dir"] "appendonly.aof"]
    catch {exec rm -rf $rdb}
    catch {exec rm -rf $aof}
}

proc kill_server config {
    # nothing to kill when running against external server
    if {$::external} return

    # nevermind if its already dead
    if {![is_alive $config]} {
        # Check valgrind errors if needed
        if {$::valgrind} {
            check_valgrind_errors [dict get $config stderr]
        }
        return
    }
    set pid [dict get $config pid]

    # check for leaks
    if {![dict exists $config "skipleaks"]} {
        catch {
            if {[string match {*Darwin*} [exec uname -a]]} {
                tags {"leaks"} {
                    test "Check for memory leaks (pid $pid)" {
                        set output {0 leaks}
                        catch {exec leaks $pid} output option
                        # In a few tests we kill the server process, so leaks will not find it.
                        # It'll exits with exit code >1 on error, so we ignore these.
                        if {[dict exists $option -errorcode]} {
                            set details [dict get $option -errorcode]
                            if {[lindex $details 0] eq "CHILDSTATUS"} {
                                  set status [lindex $details 2]
                                  if {$status > 1} {
                                      set output "0 leaks"
                                  }
                            }
                        }
                        set output
                    } {*0 leaks*}
                }
            }
        }
    }

    # kill server and wait for the process to be totally exited
    send_data_packet $::test_server_fd server-killing $pid
    catch {exec kill $pid}
    # Node might have been stopped in the test
    catch {exec kill -SIGCONT $pid}
    if {$::valgrind} {
        set max_wait 60000
    } else {
        set max_wait 10000
    }
    while {[is_alive $config]} {
        incr wait 10

        if {$wait >= $max_wait} {
            puts "Forcing process $pid to exit..."
            catch {exec kill -KILL $pid}
        } elseif {$wait % 1000 == 0} {
            puts "Waiting for process $pid to exit..."
        }
        after 10
    }

    # Check valgrind errors if needed
    if {$::valgrind} {
        check_valgrind_errors [dict get $config stderr]
    }

    # Remove this pid from the set of active pids in the test server.
    send_data_packet $::test_server_fd server-killed $pid
}

proc is_alive config {
    set pid [dict get $config pid]
    if {[catch {exec kill -0 $pid} err]} {
        return 0
    } else {
        return 1
    }
}

proc ping_server {host port} {
    set retval 0
    if {[catch {
        if {$::tls} {
            set fd [::tls::socket $host $port] 
        } else {
            set fd [socket $host $port]
        }
        fconfigure $fd -translation binary
        puts $fd "PING\r\n"
        flush $fd
        set reply [gets $fd]
        if {[string range $reply 0 0] eq {+} ||
            [string range $reply 0 0] eq {-}} {
            set retval 1
        }
        close $fd
    } e]} {
        if {$::verbose} {
            puts -nonewline "."
        }
    } else {
        if {$::verbose} {
            puts -nonewline "ok"
        }
    }
    return $retval
}

# Return 1 if the server at the specified addr is reachable by PING, otherwise
# returns 0. Performs a try every 50 milliseconds for the specified number
# of retries.
proc server_is_up {host port retrynum} {
    after 10 ;# Use a small delay to make likely a first-try success.
    set retval 0
    while {[incr retrynum -1]} {
        if {[catch {ping_server $host $port} ping]} {
            set ping 0
        }
        if {$ping} {return 1}
        after 50
    }
    return 0
}

# Check if current ::tags match requested tags. If ::allowtags are used,
# there must be some intersection. If ::denytags are used, no intersection
# is allowed. Returns 1 if tags are acceptable or 0 otherwise, in which
# case err_return names a return variable for the message to be logged.
proc tags_acceptable {err_return} {
    upvar $err_return err

    # If tags are whitelisted, make sure there's match
    if {[llength $::allowtags] > 0} {
        set matched 0
        foreach tag $::allowtags {
            if {[lsearch $::tags $tag] >= 0} {
                incr matched
            }
        }
        if {$matched < 1} {
            set err "Tag: none of the tags allowed"
            return 0
        }
    }

    foreach tag $::denytags {
        if {[lsearch $::tags $tag] >= 0} {
            set err "Tag: $tag denied"
            return 0
        }
    }

    return 1
}

# doesn't really belong here, but highly coupled to code in start_server
proc tags {tags code} {
    # If we 'tags' contain multiple tags, quoted and seperated by spaces,
    # we want to get rid of the quotes in order to have a proper list
    set tags [string map { \" "" } $tags]
    set ::tags [concat $::tags $tags]
    if {![tags_acceptable err]} {
        incr ::num_aborted
        send_data_packet $::test_server_fd ignore $err
        set ::tags [lrange $::tags 0 end-[llength $tags]]
        return
    }
    uplevel 1 $code
    set ::tags [lrange $::tags 0 end-[llength $tags]]
}

# Write the configuration in the dictionary 'config' in the specified
# file name.
proc create_server_config_file {filename config} {
    set fp [open $filename w+]
    foreach directive [dict keys $config] {
        puts -nonewline $fp "$directive "
        puts $fp [dict get $config $directive]
    }
    close $fp
}

proc spawn_server {config_file stdout stderr} {
    if {$::valgrind} {
        set pid [exec valgrind --track-origins=yes --trace-children=yes --suppressions=[pwd]/src/valgrind.sup --show-reachable=no --show-possibly-lost=no --leak-check=full src/redis-server $config_file >> $stdout 2>> $stderr &]
    } elseif ($::stack_logging) {
        set pid [exec /usr/bin/env MallocStackLogging=1 MallocLogFile=/tmp/malloc_log.txt src/redis-server $config_file >> $stdout 2>> $stderr &]
    } else {
        set pid [exec src/redis-server $config_file >> $stdout 2>> $stderr &]
    }

    if {$::wait_server} {
        set msg "server started PID: $pid. press any key to continue..."
        puts $msg
        read stdin 1
    }

    # Tell the test server about this new instance.
    send_data_packet $::test_server_fd server-spawned $pid
    return $pid
}

# Wait for actual startup, return 1 if port is busy, 0 otherwise
proc wait_server_started {config_file stdout pid} {
    set checkperiod 100; # Milliseconds
    set maxiter [expr {120*1000/$checkperiod}] ; # Wait up to 2 minutes.
    set port_busy 0
    while 1 {
        if {[regexp -- " PID: $pid" [exec cat $stdout]]} {
            break
        }
        after $checkperiod
        incr maxiter -1
        if {$maxiter == 0} {
            start_server_error $config_file "No PID detected in log $stdout"
            puts "--- LOG CONTENT ---"
            puts [exec cat $stdout]
            puts "-------------------"
            break
        }

        # Check if the port is actually busy and the server failed
        # for this reason.
        if {[regexp {Failed listening on port} [exec cat $stdout]]} {
            set port_busy 1
            break
        }
    }
    return $port_busy
}

proc dump_server_log {srv} {
    set pid [dict get $srv "pid"]
    puts "\n===== Start of server log (pid $pid) =====\n"
    puts [exec cat [dict get $srv "stdout"]]
    puts "===== End of server log (pid $pid) =====\n"
}

proc start_server {options {code undefined}} {
    # setup defaults
    set baseconfig "default.conf"
    set overrides {}
    set omit {}
    set tags {}
    set keep_persistence false

    # parse options
    foreach {option value} $options {
        switch $option {
            "config" {
                set baseconfig $value
            }
            "overrides" {
                set overrides $value
            }
            "omit" {
                set omit $value
            }
            "tags" {
                # If we 'tags' contain multiple tags, quoted and seperated by spaces,
                # we want to get rid of the quotes in order to have a proper list
                set tags [string map { \" "" } $value]
                set ::tags [concat $::tags $tags]
            }
            "keep_persistence" {
                set keep_persistence $value
            }
            default {
                error "Unknown option $option"
            }
        }
    }

    # We skip unwanted tags
    if {![tags_acceptable err]} {
        incr ::num_aborted
        send_data_packet $::test_server_fd ignore $err
        set ::tags [lrange $::tags 0 end-[llength $tags]]
        return
    }

    # If we are running against an external server, we just push the
    # host/port pair in the stack the first time
    if {$::external} {
        if {[llength $::servers] == 0} {
            set srv {}
            dict set srv "host" $::host
            dict set srv "port" $::port
            set client [redis $::host $::port 0 $::tls]
            dict set srv "client" $client
            $client select 9

            set config {}
            dict set config "port" $::port
            dict set srv "config" $config

            # append the server to the stack
            lappend ::servers $srv
        }
        r flushall
        if {[catch {set retval [uplevel 1 $code]} error]} {
            if {$::durable} {
                set msg [string range $error 10 end]
                lappend details $msg
                lappend details $::errorInfo
                lappend ::tests_failed $details

                incr ::num_failed
                send_data_packet $::test_server_fd err [join $details "\n"]
            } else {
                # Re-raise, let handler up the stack take care of this.
                error $error $::errorInfo
            }
        }
        set ::tags [lrange $::tags 0 end-[llength $tags]]
        return
    }

    set data [split [exec cat "tests/assets/$baseconfig"] "\n"]
    set config {}
    if {$::tls} {
        dict set config "tls-cert-file" [format "%s/tests/tls/server.crt" [pwd]]
        dict set config "tls-key-file" [format "%s/tests/tls/server.key" [pwd]]
        dict set config "tls-client-cert-file" [format "%s/tests/tls/client.crt" [pwd]]
        dict set config "tls-client-key-file" [format "%s/tests/tls/client.key" [pwd]]
        dict set config "tls-dh-params-file" [format "%s/tests/tls/redis.dh" [pwd]]
        dict set config "tls-ca-cert-file" [format "%s/tests/tls/ca.crt" [pwd]]
        dict set config "loglevel" "debug"
    }
    foreach line $data {
        if {[string length $line] > 0 && [string index $line 0] ne "#"} {
            set elements [split $line " "]
            set directive [lrange $elements 0 0]
            set arguments [lrange $elements 1 end]
            dict set config $directive $arguments
        }
    }

    # use a different directory every time a server is started
    dict set config dir [tmpdir server]

    # start every server on a different port
    set port [find_available_port $::baseport $::portcount]
    if {$::tls} {
        dict set config "port" 0
        dict set config "tls-port" $port
        dict set config "tls-cluster" "yes"
        dict set config "tls-replication" "yes"
    } else {
        dict set config port $port
    }

    set unixsocket [file normalize [format "%s/%s" [dict get $config "dir"] "socket"]]
    dict set config "unixsocket" $unixsocket

    # apply overrides from global space and arguments
    foreach {directive arguments} [concat $::global_overrides $overrides] {
        dict set config $directive $arguments
    }

    # remove directives that are marked to be omitted
    foreach directive $omit {
        dict unset config $directive
    }

    # write new configuration to temporary file
    set config_file [tmpfile redis.conf]
    create_server_config_file $config_file $config

    set stdout [format "%s/%s" [dict get $config "dir"] "stdout"]
    set stderr [format "%s/%s" [dict get $config "dir"] "stderr"]

    # if we're inside a test, write the test name to the server log file
    if {[info exists ::cur_test]} {
        set fd [open $stdout "a+"]
        puts $fd "### Starting server for test $::cur_test"
        close $fd
    }

    # We need a loop here to retry with different ports.
    set server_started 0
    while {$server_started == 0} {
        if {$::verbose} {
            puts -nonewline "=== ($tags) Starting server ${::host}:${port} "
        }

        send_data_packet $::test_server_fd "server-spawning" "port $port"

        set pid [spawn_server $config_file $stdout $stderr]

        # check that the server actually started
        set port_busy [wait_server_started $config_file $stdout $pid]

        # Sometimes we have to try a different port, even if we checked
        # for availability. Other test clients may grab the port before we
        # are able to do it for example.
        if {$port_busy} {
            puts "Port $port was already busy, trying another port..."
            set port [find_available_port $::baseport $::portcount]
            if {$::tls} {
                dict set config "tls-port" $port
            } else {
                dict set config port $port
            }
            create_server_config_file $config_file $config

            # Truncate log so wait_server_started will not be looking at
            # output of the failed server.
            close [open $stdout "w"]

            continue; # Try again
        }

        if {$::valgrind} {set retrynum 1000} else {set retrynum 100}
        if {$code ne "undefined"} {
            set serverisup [server_is_up $::host $port $retrynum]
        } else {
            set serverisup 1
        }

        if {$::verbose} {
            puts ""
        }

        if {!$serverisup} {
            set err {}
            append err [exec cat $stdout] "\n" [exec cat $stderr]
            start_server_error $config_file $err
            return
        }
        set server_started 1
    }

    # setup properties to be able to initialize a client object
    set port_param [expr $::tls ? {"tls-port"} : {"port"}]
    set host $::host
    if {[dict exists $config bind]} { set host [dict get $config bind] }
    if {[dict exists $config $port_param]} { set port [dict get $config $port_param] }

    # setup config dict
    dict set srv "config_file" $config_file
    dict set srv "config" $config
    dict set srv "pid" $pid
    dict set srv "host" $host
    dict set srv "port" $port
    dict set srv "stdout" $stdout
    dict set srv "stderr" $stderr
    dict set srv "unixsocket" $unixsocket

    # if a block of code is supplied, we wait for the server to become
    # available, create a client object and kill the server afterwards
    if {$code ne "undefined"} {
        set line [exec head -n1 $stdout]
        if {[string match {*already in use*} $line]} {
            error_and_quit $config_file $line
        }

        while 1 {
            # check that the server actually started and is ready for connections
            if {[count_message_lines $stdout "Ready to accept"] > 0} {
                break
            }
            after 10
        }

        # append the server to the stack
        lappend ::servers $srv

        # connect client (after server dict is put on the stack)
        reconnect

        # remember previous num_failed to catch new errors
        set prev_num_failed $::num_failed

        # execute provided block
        set num_tests $::num_tests
        if {[catch { uplevel 1 $code } error]} {
            set backtrace $::errorInfo

            # fetch srv back from the server list, in case it was restarted by restart_server (new PID)
            set srv [lindex $::servers end]

            # pop the server object
            set ::servers [lrange $::servers 0 end-1]

            # Kill the server without checking for leaks
            dict set srv "skipleaks" 1
            kill_server $srv

            # Print warnings from log
            puts [format "\nLogged warnings (pid %d):" [dict get $srv "pid"]]
            set warnings [warnings_from_file [dict get $srv "stdout"]]
            if {[string length $warnings] > 0} {
                puts "$warnings"
            } else {
                puts "(none)"
            }
            puts ""

            if {$::durable} {
                set msg [string range $error 10 end]
                lappend details $msg
                lappend details $backtrace
                lappend ::tests_failed $details

                incr ::num_failed
                send_data_packet $::test_server_fd err [join $details "\n"]
            } else {
                # Re-raise, let handler up the stack take care of this.
                error $error $backtrace
            }
        } else {
            if {$::dump_logs && $prev_num_failed != $::num_failed} {
                dump_server_log $srv
            }
        }

        # fetch srv back from the server list, in case it was restarted by restart_server (new PID)
        set srv [lindex $::servers end]

        # Don't do the leak check when no tests were run
        if {$num_tests == $::num_tests} {
            dict set srv "skipleaks" 1
        }

        # pop the server object
        set ::servers [lrange $::servers 0 end-1]

        set ::tags [lrange $::tags 0 end-[llength $tags]]
        kill_server $srv
        if {!$keep_persistence} {
            clean_persistence $srv
        }
        set _ ""
    } else {
        set ::tags [lrange $::tags 0 end-[llength $tags]]
        set _ $srv
    }
}

proc restart_server {level wait_ready rotate_logs} {
    set srv [lindex $::servers end+$level]
    kill_server $srv

    set pid [dict get $srv "pid"]
    set stdout [dict get $srv "stdout"]
    set stderr [dict get $srv "stderr"]
    if {$rotate_logs} {
        set ts [clock format [clock seconds] -format %y%m%d%H%M%S]
        file rename $stdout $stdout.$ts.$pid
        file rename $stderr $stderr.$ts.$pid
    }
    set prev_ready_count [count_message_lines $stdout "Ready to accept"]

    # if we're inside a test, write the test name to the server log file
    if {[info exists ::cur_test]} {
        set fd [open $stdout "a+"]
        puts $fd "### Restarting server for test $::cur_test"
        close $fd
    }

    set config_file [dict get $srv "config_file"]

    set pid [spawn_server $config_file $stdout $stderr]

    # check that the server actually started
    wait_server_started $config_file $stdout $pid

    # update the pid in the servers list
    dict set srv "pid" $pid
    # re-set $srv in the servers list
    lset ::servers end+$level $srv

    if {$wait_ready} {
        while 1 {
            # check that the server actually started and is ready for connections
            if {[count_message_lines $stdout "Ready to accept"] > $prev_ready_count} {
                break
            }
            after 10
        }
    }
    reconnect $level
}
