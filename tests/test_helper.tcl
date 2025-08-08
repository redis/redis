# Redis test suite.
#
# Copyright (C) 2014-Present, Redis Ltd.
# All Rights reserved.
#
# Licensed under your choice of (a) the Redis Source Available License 2.0
# (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
# GNU Affero General Public License v3 (AGPLv3).

package require Tcl 8.5

set tcl_precision 17
source tests/support/redis.tcl
source tests/support/aofmanifest.tcl
source tests/support/server.tcl
source tests/support/cluster_util.tcl
source tests/support/tmpfile.tcl
source tests/support/test.tcl
source tests/support/util.tcl

set dir [pwd]
set ::all_tests []

set test_dirs {
    unit
    unit/type
    unit/cluster
    integration
}

foreach test_dir $test_dirs {
    set files [glob -nocomplain $dir/tests/$test_dir/*.tcl]

    foreach file [lsort $files] {
        lappend ::all_tests $test_dir/[file root [file tail $file]]
    }
}
# Index to the next test to run in the ::all_tests list.
set ::next_test 0

set ::host 127.0.0.1
set ::port 6379; # port for external server
set ::baseport 21111; # initial port for spawned redis servers
set ::portcount 8000; # we don't wanna use more than 10000 to avoid collision with cluster bus ports
set ::traceleaks 0
set ::valgrind 0
set ::tsan 0
set ::durable 0
set ::tls 0
set ::tls_module 0
set ::stack_logging 0
set ::verbose 0
set ::quiet 0
set ::denytags {}
set ::skiptests {}
set ::skipunits {}
set ::no_latency 0
set ::allowtags {}
set ::only_tests {}
set ::single_tests {}
set ::run_solo_tests {}
set ::skip_till ""
set ::external 0; # If "1" this means, we are running against external instance
set ::file ""; # If set, runs only the tests in this comma separated list
set ::curfile ""; # Hold the filename of the current suite
set ::accurate 0; # If true runs fuzz tests with more iterations
set ::force_failure 0
set ::timeout 1200; # 20 minutes without progresses will quit the test.
set ::last_progress [clock seconds]
set ::active_servers {} ; # Pids of active Redis instances.
set ::dont_clean 0
set ::dont_pre_clean 0
set ::wait_server 0
set ::stop_on_failure 0
set ::dump_logs 0
set ::loop 0
set ::tlsdir "tests/tls"
set ::singledb 0
set ::cluster_mode 0
set ::ignoreencoding 0
set ::ignoredigest 0
set ::large_memory 0
set ::log_req_res 0
set ::force_resp3 0

# Set to 1 when we are running in client mode. The Redis test uses a
# server-client model to run tests simultaneously. The server instance
# runs the specified number of client instances that will actually run tests.
# The server is responsible of showing the result to the user, and exit with
# the appropriate exit code depending on the test outcome.
set ::client 0
set ::numclients 16

# This function is called by one of the test clients when it receives
# a "run" command from the server, with a filename as data.
# It will run the specified test source file and signal it to the
# test server when finished.
proc execute_test_file __testname {
    set path "tests/$__testname.tcl"
    set ::curfile $path
    source $path
    send_data_packet $::test_server_fd done "$__testname"
}

# This function is called by one of the test clients when it receives
# a "run_code" command from the server, with a verbatim test source code
# as argument, and an associated name.
# It will run the specified code and signal it to the test server when
# finished.
proc execute_test_code {__testname filename code} {
    set ::curfile $filename
    eval $code
    send_data_packet $::test_server_fd done "$__testname"
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

# Take an index to get a srv.
proc get_srv {level} {
    set srv [lindex $::servers end+$level]
    return $srv
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

# Returns a Redis instance by index.
proc Rn {n} {
    set level [expr -1*$n]
    return [srv $level "client"]
}

# Provide easy access to a client for an inner server. Requires a positive
# index, unlike r which uses an optional negative index.
proc R {n args} {
    [Rn $n] {*}$args
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
    set client [redis $host $port 0 $::tls]
    if {[dict exists $srv "client"]} {
        set old [dict get $srv "client"]
        $old close
    }
    dict set srv "client" $client

    # select the right db when we don't have to authenticate
    if {![dict exists $config "requirepass"] && !$::singledb} {
        $client select 9
    }

    # re-set $srv in the servers list
    lset ::servers end+$level $srv
}

proc redis_deferring_client {args} {
    set level 0
    if {[llength $args] > 0 && [string is integer [lindex $args 0]]} {
        set level [lindex $args 0]
        set args [lrange $args 1 end]
    }

    # create client that defers reading reply
    set client [redis [srv $level "host"] [srv $level "port"] 1 $::tls]

    # select the right db and read the response (OK)
    if {!$::singledb} {
        $client select 9
        $client read
    } else {
        # For timing/symmetry with the above select
        $client ping
        $client read
    }
    return $client
}

proc redis_client {args} {
    set level 0
    if {[llength $args] > 0 && [string is integer [lindex $args 0]]} {
        set level [lindex $args 0]
        set args [lrange $args 1 end]
    }

    # create client that won't defers reading reply
    set client [redis [srv $level "host"] [srv $level "port"] 0 $::tls]

    # select the right db and read the response (OK), or at least ping
    # the server if we're in a singledb mode.
    if {$::singledb} {
        $client ping
    } else {
        $client select 9
    }
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

# Get the specified field from the givens instances cluster info output.
proc CI {index field} {
    getInfoProperty [R $index cluster info] $field
}

# Test wrapped into run_solo are sent back from the client to the
# test server, so that the test server will send them again to
# clients once the clients are idle.
proc run_solo {name code} {
    if {$::numclients == 1 || $::loop || $::external} {
        # run_solo is not supported in these scenarios, just run the code.
        eval $code
        return
    }
    send_data_packet $::test_server_fd run_solo [list $name $::curfile $code]
}

proc cleanup {} {
    if {!$::quiet} {puts -nonewline "Cleanup: may take some time... "}
    flush stdout
    catch {exec rm -rf {*}[glob tests/tmp/redis.conf.*]}
    catch {exec rm -rf {*}[glob tests/tmp/server.*]}
    if {!$::quiet} {puts "OK"}
}

proc test_server_main {} {
    if {!$::dont_pre_clean} cleanup
    set tclsh [info nameofexecutable]
    # Open a listening socket, trying different ports in order to find a
    # non busy one.
    set clientport [find_available_port [expr {$::baseport - 32}] 32]
    if {!$::quiet} {
        puts "Starting test server at port $clientport"
    }
    socket -server accept_test_clients  -myaddr 127.0.0.1 $clientport

    # Start the client instances
    set ::clients_pids {}
    if {$::external} {
        set p [exec $tclsh [info script] {*}$::argv \
            --client $clientport &]
        lappend ::clients_pids $p
    } else {
        set start_port $::baseport
        set port_count [expr {$::portcount / $::numclients}]
        for {set j 0} {$j < $::numclients} {incr j} {
            set p [exec $tclsh [info script] {*}$::argv \
                --client $clientport --baseport $start_port --portcount $port_count &]
            lappend ::clients_pids $p
            incr start_port $port_count
        }
    }

    # Setup global state for the test server
    set ::idle_clients {}
    set ::active_clients {}
    array set ::active_clients_task {}
    array set ::clients_start_time {}
    set ::clients_time_history {}
    set ::failed_tests {}

    # Enter the event loop to handle clients I/O
    after 100 test_server_cron
    vwait forever
}

# This function gets called 10 times per second.
proc test_server_cron {} {
    set elapsed [expr {[clock seconds]-$::last_progress}]

    if {$elapsed > $::timeout} {
        set err "\[[colorstr red TIMEOUT]\]: clients state report follows."
        puts $err
        lappend ::failed_tests $err
        show_clients_state
        kill_clients
        force_kill_all_servers
        the_end
    }

    after 100 test_server_cron
}

proc accept_test_clients {fd addr port} {
    fconfigure $fd -encoding binary
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
# skip: a test was skipped by skipfile or individual test options.
# ignore: a test was skipped by a group tag.
# exception: there was a runtime exception while executing the test.
# done: all the specified test file was processed, this test client is
#       ready to accept a new task.
proc read_from_test_client fd {
    set bytes [gets $fd]
    set payload [read $fd $bytes]
    foreach {status data elapsed} $payload break
    set ::last_progress [clock seconds]

    if {$status eq {ready}} {
        if {!$::quiet} {
            puts "\[$status\]: $data"
        }
        signal_idle_client $fd
    } elseif {$status eq {done}} {
        set elapsed [expr {[clock seconds]-$::clients_start_time($fd)}]
        set all_tests_count [llength $::all_tests]
        set running_tests_count [expr {[llength $::active_clients]-1}]
        set completed_tests_count [expr {$::next_test-$running_tests_count}]
        puts "\[$completed_tests_count/$all_tests_count [colorstr yellow $status]\]: $data ($elapsed seconds)"
        lappend ::clients_time_history $elapsed $data
        signal_idle_client $fd
        set ::active_clients_task($fd) "(DONE) $data"
    } elseif {$status eq {ok}} {
        if {!$::quiet} {
            puts "\[[colorstr green $status]\]: $data ($elapsed ms)"
        }
        set ::active_clients_task($fd) "(OK) $data"
    } elseif {$status eq {skip}} {
        if {!$::quiet} {
            puts "\[[colorstr yellow $status]\]: $data"
        }
    } elseif {$status eq {ignore}} {
        if {!$::quiet} {
            puts "\[[colorstr cyan $status]\]: $data"
        }
    } elseif {$status eq {err}} {
        set err "\[[colorstr red $status]\]: $data"
        puts $err
        lappend ::failed_tests $err
        set ::active_clients_task($fd) "(ERR) $data"
        if {$::stop_on_failure} {
            puts -nonewline "(Test stopped, press enter to resume the tests)"
            flush stdout
            gets stdin
        }
    } elseif {$status eq {exception}} {
        puts "\[[colorstr red $status]\]: $data"
        kill_clients
        force_kill_all_servers
        exit 1
    } elseif {$status eq {testing}} {
        set ::active_clients_task($fd) "(IN PROGRESS) $data"
    } elseif {$status eq {server-spawning}} {
        set ::active_clients_task($fd) "(SPAWNING SERVER) $data"
    } elseif {$status eq {server-spawned}} {
        lappend ::active_servers $data
        set ::active_clients_task($fd) "(SPAWNED SERVER) pid:$data"
    } elseif {$status eq {server-killing}} {
        set ::active_clients_task($fd) "(KILLING SERVER) pid:$data"
    } elseif {$status eq {server-killed}} {
        set ::active_servers [lsearch -all -inline -not -exact $::active_servers $data]
        set ::active_clients_task($fd) "(KILLED SERVER) pid:$data"
    } elseif {$status eq {run_solo}} {
        lappend ::run_solo_tests $data
    } else {
        if {!$::quiet} {
            puts "\[$status\]: $data"
        }
    }
}

proc show_clients_state {} {
    # The following loop is only useful for debugging tests that may
    # enter an infinite loop.
    foreach x $::active_clients {
        if {[info exist ::active_clients_task($x)]} {
            puts "$x => $::active_clients_task($x)"
        } else {
            puts "$x => ???"
        }
    }
}

proc kill_clients {} {
    foreach p $::clients_pids {
        catch {exec kill $p}
    }
}

proc force_kill_all_servers {} {
    foreach p $::active_servers {
        puts "Killing still running Redis server $p"
        catch {exec kill -9 $p}
    }
}

proc lpop {listVar {count 1}} {
    upvar 1 $listVar l
    set ele [lindex $l 0]
    set l [lrange $l 1 end]
    set ele
}

proc lremove {listVar value} {
    upvar 1 $listVar var
    set idx [lsearch -exact $var $value]
    set var [lreplace $var $idx $idx]
}

# A new client is idle. Remove it from the list of active clients and
# if there are still test units to run, launch them.
proc signal_idle_client fd {
    # Remove this fd from the list of active clients.
    set ::active_clients \
        [lsearch -all -inline -not -exact $::active_clients $fd]

    # New unit to process?
    if {$::next_test != [llength $::all_tests]} {
        if {!$::quiet} {
            puts [colorstr bold-white "Testing [lindex $::all_tests $::next_test]"]
            set ::active_clients_task($fd) "ASSIGNED: $fd ([lindex $::all_tests $::next_test])"
        }
        set ::clients_start_time($fd) [clock seconds]
        send_data_packet $fd run [lindex $::all_tests $::next_test]
        lappend ::active_clients $fd
        incr ::next_test
        if {$::loop && $::next_test == [llength $::all_tests]} {
            set ::next_test 0
            incr ::loop -1
        }
    } elseif {[llength $::run_solo_tests] != 0 && [llength $::active_clients] == 0} {
        if {!$::quiet} {
            puts [colorstr bold-white "Testing solo test"]
            set ::active_clients_task($fd) "ASSIGNED: $fd solo test"
        }
        set ::clients_start_time($fd) [clock seconds]
        send_data_packet $fd run_code [lpop ::run_solo_tests]
        lappend ::active_clients $fd
    } else {
        lappend ::idle_clients $fd
        set ::active_clients_task($fd) "SLEEPING, no more units to assign"
        if {[llength $::active_clients] == 0} {
            the_end
        }
    }
}

# The the_end function gets called when all the test units were already
# executed, so the test finished.
proc the_end {} {
    # TODO: print the status, exit with the right exit code.
    puts "\n                   The End\n"
    puts "Execution time of different units:"
    foreach {time name} $::clients_time_history {
        puts "  $time seconds - $name"
    }
    if {[llength $::failed_tests]} {
        puts "\n[colorstr bold-red {!!! WARNING}] The following tests failed:\n"
        foreach failed $::failed_tests {
            puts "*** $failed"
        }
        if {!$::dont_clean} cleanup
        exit 1
    } else {
        puts "\n[colorstr bold-white {\o/}] [colorstr bold-green {All tests passed without errors!}]\n"
        if {!$::dont_clean} cleanup
        exit 0
    }
}

# The client is not event driven (the test server is instead) as we just need
# to read the command, execute, reply... all this in a loop.
proc test_client_main server_port {
    set ::test_server_fd [socket localhost $server_port]
    fconfigure $::test_server_fd -encoding binary
    send_data_packet $::test_server_fd ready [pid]
    while 1 {
        set bytes [gets $::test_server_fd]
        set payload [read $::test_server_fd $bytes]
        foreach {cmd data} $payload break
        if {$cmd eq {run}} {
            execute_test_file $data
        } elseif {$cmd eq {run_code}} {
            foreach {name filename code} $data break
            execute_test_code $name $filename $code
        } else {
            error "Unknown test client command: $cmd"
        }
    }
}

proc send_data_packet {fd status data {elapsed 0}} {
    set payload [list $status $data $elapsed]
    puts $fd [string length $payload]
    puts -nonewline $fd $payload
    flush $fd
}

proc print_help_screen {} {
    puts [join {
        "--valgrind         Run the test over valgrind."
        "--tsan             Run the test with thread sanitizer."
        "--durable          suppress test crashes and keep running"
        "--stack-logging    Enable OSX leaks/malloc stack logging."
        "--accurate         Run slow randomized tests for more iterations."
        "--quiet            Don't show individual tests."
        "--single <unit>    Just execute the specified unit (see next option). This option can be repeated."
        "--verbose          Increases verbosity."
        "--list-tests       List all the available test units."
        "--only <test>      Just execute the specified test by test name or tests that match <test> regexp (if <test> starts with '/'). This option can be repeated."
        "--skip-till <unit> Skip all units until (and including) the specified one."
        "--skipunit <unit>  Skip one unit."
        "--clients <num>    Number of test clients (default 16)."
        "--timeout <sec>    Test timeout in seconds (default 20 min)."
        "--force-failure    Force the execution of a test that always fails."
        "--config <k> <v>   Extra config file argument."
        "--skipfile <file>  Name of a file containing test names or regexp patterns (if <test> starts with '/') that should be skipped (one per line). This option can be repeated."
        "--skiptest <test>  Test name or regexp pattern (if <test> starts with '/') to skip. This option can be repeated."
        "--tags <tags>      Run only tests having specified tags or not having '-' prefixed tags."
        "--dont-clean       Don't delete redis log files after the run."
        "--dont-pre-clean   Don't delete existing redis log files before the run."
        "--no-latency       Skip latency measurements and validation by some tests."
        "--stop             Blocks once the first test fails."
        "--loop             Execute the specified set of tests forever."
        "--loops <count>    Execute the specified set of tests several times."
        "--wait-server      Wait after server is started (so that you can attach a debugger)."
        "--dump-logs        Dump server log on test failure."
        "--tls              Run tests in TLS mode."
        "--tls-module       Run tests in TLS mode with Redis module."
        "--host <addr>      Run tests against an external host."
        "--port <port>      TCP port to use against external host."
        "--baseport <port>  Initial port number for spawned redis servers."
        "--portcount <num>  Port range for spawned redis servers."
        "--singledb         Use a single database, avoid SELECT."
        "--cluster-mode     Run tests in cluster protocol compatible mode."
        "--ignore-encoding  Don't validate object encoding."
        "--ignore-digest    Don't use debug digest validations."
        "--large-memory     Run tests using over 100mb."
        "--help             Print this help screen."
    } "\n"]
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
    } elseif {$opt eq {--config}} {
        set arg2 [lindex $argv [expr $j+2]]
        lappend ::global_overrides $arg
        lappend ::global_overrides $arg2
        incr j 2
    } elseif {$opt eq {--log-req-res}} {
        set ::log_req_res 1
    } elseif {$opt eq {--force-resp3}} {
        set ::force_resp3 1
    } elseif {$opt eq {--skipfile}} {
        incr j
        set fp [open $arg r]
        set file_data [read $fp]
        close $fp
        set ::skiptests [concat $::skiptests [split $file_data "\n"]]
    } elseif {$opt eq {--skiptest}} {
        lappend ::skiptests $arg
        incr j
    } elseif {$opt eq {--valgrind}} {
        set ::valgrind 1
    } elseif {$opt eq {--tsan}} {
        set ::tsan 1
    } elseif {$opt eq {--stack-logging}} {
        if {[string match {*Darwin*} [exec uname -a]]} {
            set ::stack_logging 1
        }
    } elseif {$opt eq {--quiet}} {
        set ::quiet 1
    } elseif {$opt eq {--tls} || $opt eq {--tls-module}} {
        package require tls 1.6
        set ::tls 1
        ::tls::init \
            -cafile "$::tlsdir/ca.crt" \
            -certfile "$::tlsdir/client.crt" \
            -keyfile "$::tlsdir/client.key"
        if {$opt eq {--tls-module}} {
            set ::tls_module 1
        }
    } elseif {$opt eq {--host}} {
        set ::external 1
        set ::host $arg
        incr j
    } elseif {$opt eq {--port}} {
        set ::port $arg
        incr j
    } elseif {$opt eq {--baseport}} {
        set ::baseport $arg
        incr j
    } elseif {$opt eq {--portcount}} {
        set ::portcount $arg
        incr j
    } elseif {$opt eq {--accurate}} {
        set ::accurate 1
    } elseif {$opt eq {--force-failure}} {
        set ::force_failure 1
    } elseif {$opt eq {--single}} {
        lappend ::single_tests $arg
        incr j
    } elseif {$opt eq {--only}} {
        lappend ::only_tests $arg
        incr j
    } elseif {$opt eq {--skipunit}} {
        lappend ::skipunits $arg
        incr j
    } elseif {$opt eq {--skip-till}} {
        set ::skip_till $arg
        incr j
    } elseif {$opt eq {--list-tests}} {
        foreach t $::all_tests {
            puts $t
        }
        exit 0
    } elseif {$opt eq {--verbose}} {
        incr ::verbose
    } elseif {$opt eq {--client}} {
        set ::client 1
        set ::test_server_port $arg
        incr j
    } elseif {$opt eq {--clients}} {
        set ::numclients $arg
        incr j
    } elseif {$opt eq {--durable}} {
        set ::durable 1
    } elseif {$opt eq {--dont-clean}} {
        set ::dont_clean 1
    } elseif {$opt eq {--dont-pre-clean}} {
        set ::dont_pre_clean 1
    } elseif {$opt eq {--no-latency}} {
        set ::no_latency 1
    } elseif {$opt eq {--wait-server}} {
        set ::wait_server 1
    } elseif {$opt eq {--dump-logs}} {
        set ::dump_logs 1
    } elseif {$opt eq {--stop}} {
        set ::stop_on_failure 1
    } elseif {$opt eq {--loop}} {
        set ::loop 2147483647
    } elseif {$opt eq {--loops}} {
        set ::loop $arg
        incr j
    } elseif {$opt eq {--timeout}} {
        set ::timeout $arg
        incr j
    } elseif {$opt eq {--singledb}} {
        set ::singledb 1
    } elseif {$opt eq {--cluster-mode}} {
        set ::cluster_mode 1
        set ::singledb 1
    } elseif {$opt eq {--large-memory}} {
        set ::large_memory 1
    } elseif {$opt eq {--ignore-encoding}} {
        set ::ignoreencoding 1
    } elseif {$opt eq {--ignore-digest}} {
        set ::ignoredigest 1
    } elseif {$opt eq {--help}} {
        print_help_screen
        exit 0
    } else {
        puts "Wrong argument: $opt"
        exit 1
    }
}

set filtered_tests {}

# Set the filtered tests to be the short list (single_tests) if exists.
# Otherwise, we start filtering all_tests
if {[llength $::single_tests] > 0} {
    set filtered_tests $::single_tests
} else {
    set filtered_tests $::all_tests
}

# If --skip-till option was given, we populate the list of single tests
# to run with everything *after* the specified unit.
if {$::skip_till != ""} {
    set skipping 1
    foreach t $::all_tests {
        if {$skipping == 1} {
            lremove filtered_tests $t
        }
        if {$t == $::skip_till} {
            set skipping 0
        }
    }
    if {$skipping} {
        puts "test $::skip_till not found"
        exit 0
    }
}

# If --skipunits option was given, we populate the list of single tests
# to run with everything *not* in the skipunits list.
if {[llength $::skipunits] > 0} {
    foreach t $::all_tests {
        if {[lsearch $::skipunits $t] != -1} {
            lremove filtered_tests $t
        }
    }
}

# Override the list of tests with the specific tests we want to run
# in case there was some filter, that is --single, -skipunit or --skip-till options.
if {[llength $filtered_tests] < [llength $::all_tests]} {
    set ::all_tests $filtered_tests
}

proc attach_to_replication_stream_on_connection {conn} {
    r config set repl-ping-replica-period 3600
    if {$::tls} {
        set s [::tls::socket [srv $conn "host"] [srv $conn "port"]]
    } else {
        set s [socket [srv $conn "host"] [srv $conn "port"]]
    }
    fconfigure $s -translation binary
    puts -nonewline $s "SYNC\r\n"
    flush $s

    # Get the count
    while 1 {
        set count [gets $s]
        set prefix [string range $count 0 0]
        if {$prefix ne {}} break; # Newlines are allowed as PINGs.
    }
    if {$prefix ne {$}} {
        error "attach_to_replication_stream error. Received '$count' as count."
    }
    set count [string range $count 1 end]

    # Consume the bulk payload
    while {$count} {
        set buf [read $s $count]
        set count [expr {$count-[string length $buf]}]
    }
    return $s
}

proc attach_to_replication_stream {} {
    return [attach_to_replication_stream_on_connection 0]
}

proc read_from_replication_stream {s} {
    fconfigure $s -blocking 0
    set attempt 0
    while {[gets $s count] == -1} {
        if {[incr attempt] == 10} return ""
        after 100
    }
    fconfigure $s -blocking 1
    set count [string range $count 1 end]

    # Return a list of arguments for the command.
    set res {}
    for {set j 0} {$j < $count} {incr j} {
        read $s 1
        set arg [::redis::redis_bulk_read $s]
        if {$j == 0} {set arg [string tolower $arg]}
        lappend res $arg
    }
    return $res
}

proc assert_replication_stream {s patterns} {
    set errors 0
    set values_list {}
    set patterns_list {}
    for {set j 0} {$j < [llength $patterns]} {incr j} {
        set pattern [lindex $patterns $j]
        lappend patterns_list $pattern
        set value [read_from_replication_stream $s]
        lappend values_list $value
        if {![string match $pattern $value]} { incr errors }
    }

    if {$errors == 0} { return }

    set context [info frame -1]
    close_replication_stream $s ;# for fast exit
    assert_match $patterns_list $values_list "" $context
}

proc close_replication_stream {s} {
    close $s
    r config set repl-ping-replica-period 10
    return
}

# With the parallel test running multiple Redis instances at the same time
# we need a fast enough computer, otherwise a lot of tests may generate
# false positives.
# If the computer is too slow we revert the sequential test without any
# parallelism, that is, clients == 1.
proc is_a_slow_computer {} {
    set start [clock milliseconds]
    for {set j 0} {$j < 1000000} {incr j} {}
    set elapsed [expr [clock milliseconds]-$start]
    expr {$elapsed > 200}
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
    if {[is_a_slow_computer]} {
        puts "** SLOW COMPUTER ** Using a single client to avoid false positives."
        set ::numclients 1
    }

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
