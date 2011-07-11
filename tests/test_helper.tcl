# Redis test suite. Copyright (C) 2009 Salvatore Sanfilippo antirez@gmail.com
# This softare is released under the BSD License. See the COPYING file for
# more information.

set tcl_precision 17
source tests/support/redis.tcl
source tests/support/server.tcl
source tests/support/tmpfile.tcl
source tests/support/test.tcl
source tests/support/util.tcl

set ::all_tests {
    unit/printver
    unit/auth
    unit/protocol
    unit/basic
    unit/type/list
    unit/type/list-2
    unit/type/list-3
    unit/type/set
    unit/type/zset
    unit/type/hash
    unit/sort
    unit/expire
    unit/other
    unit/cas
    unit/quit
    integration/replication
    integration/replication-2
    integration/replication-3
    integration/aof
    unit/pubsub
    unit/slowlog
}
# Index to the next test to run in the ::all_tests list.
set ::next_test 0

set ::host 127.0.0.1
set ::port 16379
set ::traceleaks 0
set ::valgrind 0
set ::verbose 0
set ::denytags {}
set ::allowtags {}
set ::external 0; # If "1" this means, we are running against external instance
set ::file ""; # If set, runs only the tests in this comma separated list
set ::curfile ""; # Hold the filename of the current suite
set ::accurate 0; # If true runs fuzz tests with more iterations
set ::force_failure 0

# Set to 1 when we are running in client mode. The Redis test uses a
# server-client model to run tests simultaneously. The server instance
# runs the specified number of client instances that will actually run tests.
# The server is responsible of showing the result to the user, and exit with
# the appropriate exit code depending on the test outcome.
set ::client 0
set ::numclients 16

proc execute_tests name {
    set path "tests/$name.tcl"
    set ::curfile $path
    source $path
    send_data_packet $::test_server_fd done "$name"
}

# Setup a list to hold a stack of server configs. When calls to start_server
# are nested, use "srv 0 pid" to get the pid of the inner server. To access
# outer servers, use "srv -1 pid" etcetera.
set ::servers {}
proc srv {args} {
    set level 0
    if {[string is integer [lindex $args 0]]} {
        set level [lindex $args 0]
        set property [lindex $args 1]
    } else {
        set property [lindex $args 0]
    }
    set srv [lindex $::servers end+$level]
    dict get $srv $property
}

# Provide easy access to the client for the inner server. It's possible to
# prepend the argument list with a negative level to access clients for
# servers running in outer blocks.
proc r {args} {
    set level 0
    if {[string is integer [lindex $args 0]]} {
        set level [lindex $args 0]
        set args [lrange $args 1 end]
    }
    [srv $level "client"] {*}$args
}

proc reconnect {args} {
    set level [lindex $args 0]
    if {[string length $level] == 0 || ![string is integer $level]} {
        set level 0
    }

    set srv [lindex $::servers end+$level]
    set host [dict get $srv "host"]
    set port [dict get $srv "port"]
    set config [dict get $srv "config"]
    set client [redis $host $port]
    dict set srv "client" $client

    # select the right db when we don't have to authenticate
    if {![dict exists $config "requirepass"]} {
        $client select 9
    }

    # re-set $srv in the servers list
    set ::servers [lreplace $::servers end+$level 1 $srv]
}

proc redis_deferring_client {args} {
    set level 0
    if {[llength $args] > 0 && [string is integer [lindex $args 0]]} {
        set level [lindex $args 0]
        set args [lrange $args 1 end]
    }

    # create client that defers reading reply
    set client [redis [srv $level "host"] [srv $level "port"] 1]

    # select the right db and read the response (OK)
    $client select 9
    $client read
    return $client
}

# Provide easy access to INFO properties. Same semantic as "proc r".
proc s {args} {
    set level 0
    if {[string is integer [lindex $args 0]]} {
        set level [lindex $args 0]
        set args [lrange $args 1 end]
    }
    status [srv $level "client"] [lindex $args 0]
}

proc cleanup {} {
    puts -nonewline "Cleanup: warning may take some time... "
    flush stdout
    catch {exec rm -rf {*}[glob tests/tmp/redis.conf.*]}
    catch {exec rm -rf {*}[glob tests/tmp/server.*]}
    puts "OK"
}

proc test_server_main {} {
    cleanup
    # Open a listening socket, trying different ports in order to find a
    # non busy one.
    set port 11111
    while 1 {
        puts "Starting test server at port $port"
        if {[catch {socket -server accept_test_clients $port} e]} {
            if {[string match {*address already in use*} $e]} {
                if {$port == 20000} {
                    puts "Can't find an available TCP port for test server."
                    exit 1
                } else {
                    incr port
                }
            } else {
                puts "Fatal error starting test server: $e"
                exit 1
            }
        } else {
            break
        }
    }

    # Start the client instances
    set ::clients_pids {}
    for {set j 0} {$j < $::numclients} {incr j} {
        set p [exec tclsh8.5 [info script] {*}$::argv \
            --client $port --port [expr {$::port+($j*10)}] &]
        lappend ::clients_pids $p
    }

    # Setup global state for the test server
    set ::idle_clients {}
    set ::active_clients {}
    array set ::clients_start_time {}
    set ::clients_time_history {}
    set ::failed_tests {}

    # Enter the event loop to handle clients I/O
    after 100 test_server_cron
    vwait forever
}

# This function gets called 10 times per second, for now does nothing but
# may be used in the future in order to detect test clients taking too much
# time to execute the task.
proc test_server_cron {} {
}

proc accept_test_clients {fd addr port} {
    fileevent $fd readable [list read_from_test_client $fd]
}

# This is the readable handler of our test server. Clients send us messages
# in the form of a status code such and additional data. Supported
# status types are:
#
# ready: the client is ready to execute the command. Only sent at client
#        startup. The server will queue the client FD in the list of idle
#        clients.
# testing: just used to signal that a given test started.
# ok: a test was executed with success.
# err: a test was executed with an error.
# exception: there was a runtime exception while executing the test.
# done: all the specified test file was processed, this test client is
#       ready to accept a new task.
proc read_from_test_client fd {
    set bytes [gets $fd]
    set payload [read $fd $bytes]
    foreach {status data} $payload break
    if {$status eq {ready}} {
        puts "\[$status\]: $data"
        signal_idle_client $fd
    } elseif {$status eq {done}} {
        set elapsed [expr {[clock seconds]-$::clients_start_time($fd)}]
        puts "\[[colorstr yellow $status]\]: $data ($elapsed seconds)"
        puts "+++ [expr {[llength $::active_clients]-1}] units still in execution."
        lappend ::clients_time_history $elapsed $data
        signal_idle_client $fd
    } elseif {$status eq {ok}} {
        puts "\[[colorstr green $status]\]: $data"
    } elseif {$status eq {err}} {
        set err "\[[colorstr red $status]\]: $data"
        puts $err
        lappend ::failed_tests $err
    } elseif {$status eq {exception}} {
        puts "\[[colorstr red $status]\]: $data"
        foreach p $::clients_pids {
            catch {exec kill -9 $p}
        }
        exit 1
    } elseif {$status eq {testing}} {
        # No op
    } else {
        puts "\[$status\]: $data"
    }
}

# A new client is idle. Remove it from the list of active clients and
# if there are still test units to run, launch them.
proc signal_idle_client fd {
    # Remove this fd from the list of active clients.
    set ::active_clients \
        [lsearch -all -inline -not -exact $::active_clients $fd]
    # New unit to process?
    if {$::next_test != [llength $::all_tests]} {
        puts [colorstr bold-white "Testing [lindex $::all_tests $::next_test]"]
        set ::clients_start_time($fd) [clock seconds]
        send_data_packet $fd run [lindex $::all_tests $::next_test]
        lappend ::active_clients $fd
        incr ::next_test
    } else {
        lappend ::idle_clients $fd
        if {[llength $::active_clients] == 0} {
            the_end
        }
    }
}

# The the_end funciton gets called when all the test units were already
# executed, so the test finished.
proc the_end {} {
    # TODO: print the status, exit with the rigth exit code.
    puts "\n                   The End\n"
    puts "Execution time of different units:"
    foreach {time name} $::clients_time_history {
        puts "  $time seconds - $name"
    }
    if {[llength $::failed_tests]} {
        puts "!!! WARNING: The following tests failed\n"
        foreach failed $::failed_tests {
            puts "*** $failed"
        }
        exit 1
    } else {
        puts "\n[colorstr bold-white {\o/}] [colorstr bold-green {All tests passed without errors!}]\n"
        exit 0
    }
}

# The client is not even driven (the test server is instead) as we just need
# to read the command, execute, reply... all this in a loop.
proc test_client_main server_port {
    set ::test_server_fd [socket localhost $server_port]
    send_data_packet $::test_server_fd ready [pid]
    while 1 {
        set bytes [gets $::test_server_fd]
        set payload [read $::test_server_fd $bytes]
        foreach {cmd data} $payload break
        if {$cmd eq {run}} {
            execute_tests $data
        } else {
            error "Unknown test client command: $cmd"
        }
    }
}

proc send_data_packet {fd status data} {
    set payload [list $status $data]
    puts $fd [string length $payload]
    puts -nonewline $fd $payload
    flush $fd
}

# parse arguments
for {set j 0} {$j < [llength $argv]} {incr j} {
    set opt [lindex $argv $j]
    set arg [lindex $argv [expr $j+1]]
    if {$opt eq {--tags}} {
        foreach tag $arg {
            if {[string index $tag 0] eq "-"} {
                lappend ::denytags [string range $tag 1 end]
            } else {
                lappend ::allowtags $tag
            }
        }
        incr j
    } elseif {$opt eq {--valgrind}} {
        set ::valgrind 1
    } elseif {$opt eq {--file}} {
        set ::file $arg
        incr j
    } elseif {$opt eq {--host}} {
        set ::external 1
        set ::host $arg
        incr j
    } elseif {$opt eq {--port}} {
        set ::port $arg
        incr j
    } elseif {$opt eq {--verbose}} {
        set ::verbose 1
    } elseif {$opt eq {--accurate}} {
        set ::accurate 1
    } elseif {$opt eq {--force-failure}} {
        set ::force_failure 1
    } elseif {$opt eq {--single}} {
        set ::all_tests $arg
        incr j
    } elseif {$opt eq {--list-tests}} {
        foreach t $::all_tests {
            puts $t
        }
        exit 0
    } elseif {$opt eq {--client}} {
        set ::client 1
        set ::test_server_port $arg
        incr j
    } elseif {$opt eq {--help}} {
        puts "TODO print an help screen"
        exit 0
    } else {
        puts "Wrong argument: $opt"
        exit 1
    }
}

if {$::client} {
    if {[catch { test_client_main $::test_server_port } err]} {
        set estr "Executing test client: $err.\n$::errorInfo"
        if {[catch {send_data_packet $::test_server_fd exception $estr}]} {
            puts $estr
        }
        exit 1
    }
} else {
    if {[catch { test_server_main } err]} {
        if {[string length $err] > 0} {
            # only display error when not generated by the test suite
            if {$err ne "exception"} {
                puts $::errorInfo
            }
            exit 1
        }
    }
}
