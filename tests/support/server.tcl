set ::global_overrides {}
set ::tags {}
set ::valgrind_errors {}

proc error_and_quit {config_file error} {
    puts "!!COULD NOT START REDIS-SERVER\n"
    puts "CONFIGURATION:"
    puts [exec cat $config_file]
    puts "\nERROR:"
    puts [string trim $error]
    exit 1
}

proc check_valgrind_errors stderr {
    set fd [open $stderr]
    set buf [read $fd]
    close $fd

    if {![regexp -- {ERROR SUMMARY: 0 errors} $buf] ||
        (![regexp -- {definitely lost: 0 bytes} $buf] &&
         ![regexp -- {no leaks are possible} $buf])} {
        send_data_packet $::test_server_fd err "Valgrind error: $buf\n"
    }
}

proc kill_server config {
    # nothing to kill when running against external server
    if {$::external} return

    # nevermind if its already dead
    if {![is_alive $config]} { return }
    set pid [dict get $config pid]

    # check for leaks
    if {![dict exists $config "skipleaks"]} {
        catch {
            if {[string match {*Darwin*} [exec uname -a]]} {
                tags {"leaks"} {
                    test "Check for memory leaks (pid $pid)" {
                        exec leaks $pid
                    } {*0 leaks*}
                }
            }
        }
    }

    # kill server and wait for the process to be totally exited
    while {[is_alive $config]} {
        if {[incr wait 10] % 1000 == 0} {
            puts "Waiting for process $pid to exit..."
        }
        catch {exec kill $pid}
        after 10
    }

    # Check valgrind errors if needed
    if {$::valgrind} {
        check_valgrind_errors [dict get $config stderr]
    }
}

proc is_alive config {
    set pid [dict get $config pid]
    if {[catch {exec ps -p $pid} err]} {
        return 0
    } else {
        return 1
    }
}

proc ping_server {host port} {
    set retval 0
    if {[catch {
        set fd [socket $::host $::port]
        fconfigure $fd -translation binary
        puts $fd "PING\r\n"
        flush $fd
        set reply [gets $fd]
        if {[string range $reply 0 4] eq {+PONG} ||
            [string range $reply 0 3] eq {-ERR}} {
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

# doesn't really belong here, but highly coupled to code in start_server
proc tags {tags code} {
    set ::tags [concat $::tags $tags]
    uplevel 1 $code
    set ::tags [lrange $::tags 0 end-[llength $tags]]
}

proc start_server {options {code undefined}} {
    # If we are runnign against an external server, we just push the
    # host/port pair in the stack the first time
    if {$::external} {
        if {[llength $::servers] == 0} {
            set srv {}
            dict set srv "host" $::host
            dict set srv "port" $::port
            set client [redis $::host $::port]
            dict set srv "client" $client
            $client select 9

            # append the server to the stack
            lappend ::servers $srv
        }
        uplevel 1 $code
        return
    }

    # setup defaults
    set baseconfig "default.conf"
    set overrides {}
    set tags {}

    # parse options
    foreach {option value} $options {
        switch $option {
            "config" {
                set baseconfig $value }
            "overrides" {
                set overrides $value }
            "tags" {
                set tags $value
                set ::tags [concat $::tags $value] }
            default {
                error "Unknown option $option" }
        }
    }

    set data [split [exec cat "tests/assets/$baseconfig"] "\n"]
    set config {}
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
    dict set config port [incr ::port]

    # apply overrides from global space and arguments
    foreach {directive arguments} [concat $::global_overrides $overrides] {
        dict set config $directive $arguments
    }
    
    # write new configuration to temporary file
    set config_file [tmpfile redis.conf]
    set fp [open $config_file w+]
    foreach directive [dict keys $config] {
        puts -nonewline $fp "$directive "
        puts $fp [dict get $config $directive]
    }
    close $fp

    set stdout [format "%s/%s" [dict get $config "dir"] "stdout"]
    set stderr [format "%s/%s" [dict get $config "dir"] "stderr"]

    if {$::valgrind} {
        exec valgrind --suppressions=src/valgrind.sup src/redis-server $config_file > $stdout 2> $stderr &
    } else {
        exec src/redis-server $config_file > $stdout 2> $stderr &
    }
    
    # check that the server actually started
    # ugly but tries to be as fast as possible...
    set retrynum 100
    set serverisup 0

    if {$::verbose} {
        puts -nonewline "=== ($tags) Starting server ${::host}:${::port} "
    }

    after 10
    if {$code ne "undefined"} {
        while {[incr retrynum -1]} {
            catch {
                if {[ping_server $::host $::port]} {
                    set serverisup 1
                }
            }
            if {$serverisup} break
            after 50
        }
    } else {
        set serverisup 1
    }

    if {$::verbose} {
        puts ""
    }

    if {!$serverisup} {
        error_and_quit $config_file [exec cat $stderr]
    }
    
    # find out the pid
    while {![info exists pid]} {
        regexp {\[(\d+)\]} [exec cat $stdout] _ pid
        after 100
    }

    # setup properties to be able to initialize a client object
    set host $::host
    set port $::port
    if {[dict exists $config bind]} { set host [dict get $config bind] }
    if {[dict exists $config port]} { set port [dict get $config port] }

    # setup config dict
    dict set srv "config_file" $config_file
    dict set srv "config" $config
    dict set srv "pid" $pid
    dict set srv "host" $host
    dict set srv "port" $port
    dict set srv "stdout" $stdout
    dict set srv "stderr" $stderr

    # if a block of code is supplied, we wait for the server to become
    # available, create a client object and kill the server afterwards
    if {$code ne "undefined"} {
        set line [exec head -n1 $stdout]
        if {[string match {*already in use*} $line]} {
            error_and_quit $config_file $line
        }

        while 1 {
            # check that the server actually started and is ready for connections
            if {[exec cat $stdout | grep "ready to accept" | wc -l] > 0} {
                break
            }
            after 10
        }

        # append the server to the stack
        lappend ::servers $srv

        # connect client (after server dict is put on the stack)
        reconnect

        # execute provided block
        set num_tests $::num_tests
        if {[catch { uplevel 1 $code } error]} {
            set backtrace $::errorInfo

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

            error $error $backtrace
        }

        # Don't do the leak check when no tests were run
        if {$num_tests == $::num_tests} {
            dict set srv "skipleaks" 1
        }

        # pop the server object
        set ::servers [lrange $::servers 0 end-1]

        set ::tags [lrange $::tags 0 end-[llength $tags]]
        kill_server $srv
    } else {
        set ::tags [lrange $::tags 0 end-[llength $tags]]
        set _ $srv
    }
}
