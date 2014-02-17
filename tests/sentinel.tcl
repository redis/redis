# Sentinel test suite. Copyright (C) 2014 Salvatore Sanfilippo antirez@gmail.com
# This softare is released under the BSD License. See the COPYING file for
# more information.

package require Tcl 8.5

set tcl_precision 17
source tests/support/redis.tcl
source tests/support/util.tcl
source tests/support/server.tcl
source tests/support/test.tcl

set ::verbose 0
set ::sentinel_instances {}
set ::redis_instances {}
set ::sentinel_base_port 20000
set ::redis_base_port 30000
set ::instances_count 5 ; # How many Sentinels / Instances we use at max
set ::pids {} ; # We kill everything at exit
set ::dirs {} ; # We remove all the temp dirs at exit

if {[catch {cd tests/sentinel-tmp}]} {
    puts "tests/sentinel-tmp directory not found."
    puts "Please run this test from the Redis source root."
    exit 1
}

# Spawn a redis or sentinel instance, depending on 'type'.
proc spawn_instance {type base_port count} {
    for {set j 0} {$j < $count} {incr j} {
        set port [find_available_port $base_port]
        incr base_port
        puts "Starting $type #$j at port $port"

        # Create a directory for this Sentinel.
        set dirname "${type}_${j}"
        lappend ::dirs $dirname
        catch {exec rm -rf $dirname}
        file mkdir $dirname

        # Write the Sentinel config file.
        set cfgfile [file join $dirname $type.conf]
        set cfg [open $cfgfile w]
        puts $cfg "port $port"
        puts $cfg "dir ./$dirname"
        puts $cfg "logfile log.txt"
        close $cfg

        # Finally exec it and remember the pid for later cleanup.
        set sentinel_pid [exec ../../src/redis-sentinel $cfgfile &]
        lappend ::pids $sentinel_pid

        # Check availability
        if {[server_is_up 127.0.0.1 $port 100] == 0} {
            abort_sentinel_test "Problems starting $type #$j: ping timeout"
        }

        # Push the instance into the right list
        lappend ${type}_instances [list \
            host 127.0.0.1 \
            port $port \
            [redis 127.0.0.1 $port] \
        ]
    }
}

proc cleanup {} {
    puts "Cleaning up..."
    foreach pid $::pids {
        catch {exec kill -9 $pid}
    }
    foreach dir $::dirs {
        catch {exec rm -rf $dir}
    }
}

proc abort_sentinel_test msg {
    puts "WARNING: Aborting the test."
    puts ">>>>>>>> $msg"
    cleanup
    exit 1
}

proc main {} {
    spawn_instance sentinel $::sentinel_base_port $::instances_count
    spawn_instance redis $::redis_base_port $::instances_count
    run_tests
    cleanup
}

# We redefine 'test' as for Sentinel we don't use the server-client
# architecture for the test, everything is sequential.
proc test {descr code} {
    puts -nonewline "> $descr: "
    flush stdout

    if {[catch {set retval [uplevel 1 $code]} error]} {
        if {[string match "assertion:*" $error]} {
            set msg [string range $error 10 end]
            puts [colorstr red $msg]
        } else {
            # Re-raise, let handler up the stack take care of this.
            error $error $::errorInfo
        }
    } else {
        puts [colorstr green OK]
    }
}

proc run_tests {} {
    set tests [lsort [glob ../sentinel-tests/*]]
    foreach test $tests {
        puts [colorstr green "### [lindex [file split $test] end]"]
        source $test
    }
}

if {[catch main e]} {
    puts $::errorInfo
    cleanup
}
