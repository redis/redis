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
        ![regexp -- {definitely lost: 0 bytes} $buf]} {
        puts "*** VALGRIND ERRORS ***"
        puts $buf
        puts "--- press enter to continue ---"
        gets stdin
    }
}

proc kill_server config {
    # nevermind if its already dead
    if {![is_alive $config]} { return }
    set pid [dict get $config pid]

    # check for leaks
    catch {
        if {[string match {*Darwin*} [exec uname -a]]} {
            test "Check for memory leaks (pid $pid)" {
                exec leaks $pid
            } {*0 leaks*}
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
        puts "Can't PING server at $host:$port... $e"
    }
    return $retval
}

set ::global_overrides {}
proc start_server {filename overrides {code undefined}} {
    set data [split [exec cat "tests/assets/$filename"] "\n"]
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
    foreach override [concat $::global_overrides $overrides] {
        set directive [lrange $override 0 0]
        set arguments [lrange $override 1 end]
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
        exec valgrind --leak-check=full ./redis-server $config_file > $stdout 2> $stderr &
        after 2000
    } else {
        exec ./redis-server $config_file > $stdout 2> $stderr &
        after 500
    }
    
    # check that the server actually started
    if {$code ne "undefined" && ![ping_server $::host $::port]} {
        error_and_quit $config_file [exec cat $stderr]
    }
    
    # find out the pid
    while {![info exists pid]} {
        regexp {^\[(\d+)\]} [exec head -n1 $stdout] _ pid
        after 100
    }

    # setup properties to be able to initialize a client object
    set host $::host
    set port $::port
    if {[dict exists $config bind]} { set host [dict get $config bind] }
    if {[dict exists $config port]} { set port [dict get $config port] }

    # setup config dict
    dict set srv "config" $config_file
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

        set client [redis $host $port]
        dict set srv "client" $client

        # select the right db when we don't have to authenticate
        if {![dict exists $config requirepass]} {
            $client select 9
        }

        # append the server to the stack
        lappend ::servers $srv
        
        # execute provided block
        catch { uplevel 1 $code } err

        # pop the server object
        set ::servers [lrange $::servers 0 end-1]
        
        kill_server $srv

        if {[string length $err] > 0} {
            puts "Error executing the suite, aborting..."
            puts $err
            exit 1
        }
    } else {
        set _ $srv
    }
}
