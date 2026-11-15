# Redis test suite.
#
# Copyright (C) 2014-Present, Redis Ltd.
# All Rights reserved.
#
# Copyright (c) 2024-present, Valkey contributors.
# All rights reserved.
#
# Licensed under your choice of (a) the Redis Source Available License 2.0
# (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
# GNU Affero General Public License v3 (AGPLv3).
#
# Portions of this file are available under BSD3 terms; see REDISCONTRIBUTIONS for more information.
#

package require Tcl 8.5-10

if {$tcl_version < 9.0} { set tcl_precision 17 }
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
    unit/moduleapi
    unit/cluster
    integration
}

foreach test_dir $test_dirs {
    set files [glob -nocomplain $dir/tests/$test_dir/*.tcl]

    foreach file [lsort $files] {
        lappend ::all_tests $test_dir/[file root [file tail $file]]
    }
}

# The cluster tests, for --cluster. They are part of ::all_tests above and run
# with everything else by default; this list just lets a run be limited to them.
set ::cluster_all_tests {}
foreach file [lsort [glob -nocomplain $dir/tests/unit/cluster/*.tcl]] {
    lappend ::cluster_all_tests unit/cluster/[file root [file tail $file]]
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
array set ::client_pid {};         # os pid of each test client, from its ready packet
array set ::client_sigusr1_trace {}; # clients that answer SIGUSR1 with a Tcl stack trace (need Tclx)
set ::in_timeout_report 0;         # true while reporting a timeout (we re-enter the event loop)
array set ::client_trace_received {}; # clients that answered a timeout report with a Tcl stack trace
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
set ::debug_defrag 0
set ::compression 0

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

proc redis_deferring_client_by_addr {host port} {
    set client [redis $host $port 1 $::tls]
    return $client
}

proc redis_client_by_addr {host port} {
    set client [redis $host $port 0 $::tls]
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

proc S {index field} {
    getInfoProperty [R $index info] $field
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
    # request_client_stack_traces pumps the event loop, which would otherwise
    # let this fire again in the middle of a report.
    if {$::in_timeout_report} return

    set elapsed [expr {[clock seconds]-$::last_progress}]

    if {$elapsed > $::timeout} {
        set ::in_timeout_report 1
        set err "\[[colorstr red TIMEOUT]\]: clients state report follows."
        puts $err
        lappend ::failed_tests $err
        show_clients_state

        # Order matters: collect the servers first. Poking a client makes it
        # unwind, and on the way out start_server kills the very servers we
        # want a crash report from (and tells us they are gone, emptying
        # ::active_servers).
        # Nothing here may throw: this runs from an "after" handler, so an
        # error would be swallowed as a background error and, with the cron no
        # longer rescheduled, leave the run hanging instead of tearing it down.
        if {[catch dump_stuck_servers e]} {
            puts "(collecting server crash reports failed: $e)"
        }
        if {[catch request_client_stack_traces e]} {
            puts "(collecting the clients' Tcl stack traces failed: $e)"
        }

        kill_clients
        force_kill_all_servers
        the_end
    }

    after 100 test_server_cron
}

# Let the event loop run until every client in $fds has reported a Tcl stack
# trace, or we give up. A report arrives as an ordinary exception/err packet,
# which read_from_test_client prints and records.
proc pump_for_client_traces {fds ticks} {
    for {set i 0} {$i < $ticks} {incr i} {
        set pending 0
        foreach fd $fds {
            if {![info exists ::client_trace_received($fd)]} {incr pending}
        }
        if {!$pending} return
        after 100
        update
    }
}

# Get a Tcl stack trace out of every active test client, so we learn where in
# the test each one hung and not just which test it was running. No client has
# reported progress for --timeout seconds, so all of them are stuck.
#
# There are two ways one reaches us. Crash-reporting the servers above usually
# unblocks a client by itself: its connection dies, the error unwinds, and the
# client's existing top-level handler reports $::errorInfo. So collect that
# first. Only a client stuck on something else -- a still-live server, an exec,
# a sleep -- has to be poked, and for that we use SIGUSR1, which the client turns
# into a Tcl error (see test_client_main): the same unwind, on demand.
proc request_client_stack_traces {} {
    set fds $::active_clients
    pump_for_client_traces $fds 30

    set poked {}
    foreach fd $fds {
        if {[info exists ::client_trace_received($fd)]} continue
        if {![info exists ::client_pid($fd)]} {
            puts "(no pid known for test client $fd, skipping its Tcl stack trace)"
            continue
        }
        set pid $::client_pid($fd)
        if {![info exists ::client_sigusr1_trace($fd)]} {
            puts "(test client $fd (pid $pid) has no Tclx, so it can't be asked for a Tcl stack trace)"
            continue
        }
        if {![is_running $pid]} {
            puts "(test client $fd (pid $pid) already exited without reporting a Tcl stack trace)"
            continue
        }
        puts "Requesting a Tcl stack trace from test client $fd (pid $pid)..."
        if {[catch {exec kill -USR1 $pid} e]} {
            puts "(couldn't signal the test client: $e)"
            continue
        }
        lappend poked $fd
    }
    if {![llength $poked]} return

    pump_for_client_traces $poked 100
    foreach fd $poked {
        if {![info exists ::client_trace_received($fd)]} {
            puts "(no Tcl stack trace came back from test client $fd; it may be running without Tclx)"
        }
    }
}

proc accept_test_clients {fd addr port} {
    fconfigure $fd -translation binary
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
# sigusr1-trace: the client answers SIGUSR1 with a Tcl stack trace (see
#       test_client_main), so a timeout report may ask it for one.
proc read_from_test_client fd {
    if {[catch {set bytes [gets $fd]}] || ![string is integer -strict $bytes] ||
        [catch {set payload [encoding convertfrom utf-8 [read $fd $bytes]]}]} {
        # The client is gone. Stop listening, otherwise the dead socket stays
        # readable and spins this handler.
        fileevent $fd readable {}
        # During a timeout report that is expected: a client exits right after
        # reporting the stack trace we asked for.
        if {$::in_timeout_report} return
        # Otherwise it died without reporting an exception (crashed, OOM,
        # killed), which is just as fatal -- don't wait for --timeout.
        set task "no state reported"
        if {[info exists ::active_clients_task($fd)]} {set task $::active_clients_task($fd)}
        puts "\[[colorstr red exception]\]: Test client $fd exited unexpectedly -- last client state: $task"
        kill_clients
        force_kill_all_servers
        exit 1
    }
    foreach {status data elapsed} $payload break
    set ::last_progress [clock seconds]

    if {$status eq {ready}} {
        if {!$::quiet} {
            puts "\[$status\]: $data"
        }
        # The payload is the client's os pid; remember it so a timeout can ask
        # this client for a Tcl stack trace.
        set ::client_pid($fd) $data
        signal_idle_client $fd
    } elseif {$status eq {done}} {
        set elapsed [expr {[clock seconds]-$::clients_start_time($fd)}]
        set all_tests_count [llength $::all_tests]
        set running_tests_count [expr {[llength $::active_clients]-1}]
        set completed_tests_count [expr {$::next_test-$running_tests_count}]
        puts "\[$completed_tests_count/$all_tests_count [colorstr yellow $status]\]: $data ($elapsed seconds)"
        lappend ::clients_time_history $elapsed $data
        if {$::in_timeout_report} {
            # A client can finish its unit once its servers are crash-reported
            # (e.g. under --durable). It has nothing left to report, and
            # signal_idle_client would hand it a new unit or call the_end, which
            # exits before the timeout report is done.
            set ::client_trace_received($fd) 1
            return
        }
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
        if {$::in_timeout_report} {
            # Same as below, for a --durable run where the client reports the
            # interrupted test as a failure rather than re-raising.
            set ::client_trace_received($fd) 1
            return
        }
        lappend ::failed_tests $err
        set ::active_clients_task($fd) "(ERR) $data"
        if {$::stop_on_failure} {
            puts -nonewline "(Test stopped, press enter to resume the tests)"
            flush stdout
            gets stdin
        }
    } elseif {$status eq {exception}} {
        puts "\[[colorstr red $status]\]: $data"
        if {$::in_timeout_report} {
            # This is the Tcl stack trace a timeout report is waiting for;
            # carry on with the report instead of tearing down here.
            set ::client_trace_received($fd) 1
            return
        }
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
    } elseif {$status eq {sigusr1-trace}} {
        set ::client_sigusr1_trace($fd) 1
    } elseif {$status eq {run_solo}} {
        lappend ::run_solo_tests $data
    } else {
        if {!$::quiet} {
            puts "\[$status\]: $data"
        }
    }
}

# Like is_alive, but a zombie counts as gone. We are not the parent of the
# servers (the stuck test client is, and it is not reaping them) nor do we reap
# the test clients until our next exec, so after dying they linger as zombies,
# which kill -0 still reports as alive.
proc is_running pid {
    if {[catch {get_proc_state $pid} st]} {return 0}
    expr {![string match Z* [string trim $st]]}
}

# Map a server pid to its log files. The test server only ever learns pids (from
# the server-spawned packet), not paths, so find the tests/tmp directory whose
# log carries that pid as a line prefix ("<pid>:M ...").
proc server_log_files_of_pid {pid} {
    set res {}
    foreach f [glob -nocomplain "tests/tmp/*/stdout"] {
        if {[catch {set fh [open $f r]}]} continue
        fconfigure $fh -translation binary; # see dump_crash_report
        # The whole file: a server restarted without rotating its log appends
        # to the same one, so its pid may first appear anywhere.
        set data [read $fh]
        close $fh
        if {![regexp -line "^$pid:" $data]} continue
        lappend res $f
        set errfile [file join [file dirname $f] stderr]
        if {[file exists $errfile]} {lappend res $errfile}
    }
    return $res
}

# Print the end of a server log: the crash report we just asked for, plus a few
# lines of context above it for what the server was doing. A timeout can leave
# many servers alive across all clients, so we don't want whole logs. A log with
# no crash report (the server died before writing one, or this is its stderr)
# gets its tail instead, which is then the only evidence there is.
proc dump_crash_report {f pid {context_lines 10} {tail_bytes 262144}} {
    if {[catch {set fh [open $f r]} e]} {
        puts "(can't open $f: $e)"
        return
    }
    # Read raw bytes: a log can hold anything (binary keys in the client list),
    # and a strict decode (Tcl 9's default) would throw mid-report.
    fconfigure $fh -translation binary
    set data [read $fh]
    close $fh
    set size [string length $data]

    set pos [string last "REDIS BUG REPORT START" $data]
    if {$pos >= 0} {
        # Back up to the start of the marker line, then context_lines more.
        for {set i 0} {$i <= $context_lines && $pos >= 0} {incr i} {
            set pos [string last "\n" $data [expr {$pos-1}]]
        }
        set start [expr {$pos+1}]
    } else {
        set start [expr {max(0, $size-$tail_bytes)}]
    }

    puts "\n===== Start of $f (pid $pid, last [expr {$size-$start}] of $size bytes) =====\n"
    if {$start} {puts "\[...$start earlier bytes skipped...\]"}
    set out [string range $data $start end]
    catch {set out [encoding convertfrom utf-8 $out]}; # else print it raw
    puts $out
    puts "===== End of $f (pid $pid) =====\n"
}

# A timeout SIGKILLs every server, throwing away the only evidence we could
# have had -- and --dump-logs only fires for a failed or excepted test, never
# for a timeout. So ask the servers to talk first.
#
# SIGSEGV makes redis log a stack trace of every one of its threads plus INFO,
# the client list and the config (printCrashReport) and then die. The handler
# runs on whichever thread takes the signal, so it works on a server whose
# event loop is wedged -- exactly the case we cannot diagnose from outside.
# kill_server already resorts to SIGSEGV for the same reason when a server
# won't exit, but the timeout path never reaches it.
proc dump_stuck_servers {} {
    set pids $::active_servers
    if {[llength $pids] == 0} return

    # Signal every server, even one whose log we can't find: the crash report
    # also unblocks a client waiting on it.
    puts "Requesting crash reports (SIGSEGV) from [llength $pids] still running server(s)..."
    foreach p $pids {
        # A stopped server (pause_process, SIGSTOP) only runs the handler once
        # continued -- kill_server does the same for the same reason.
        catch {exec kill -SIGCONT $p}
        catch {exec kill -SEGV $p}
    }

    # The handler writes the whole report before letting the process die, so the
    # log is complete once the process is gone.
    foreach p $pids {
        for {set i 0} {$i < 300 && [is_running $p]} {incr i} {after 100}
    }

    foreach p $pids {
        set files [server_log_files_of_pid $p]
        if {[llength $files] == 0} {
            puts "(no log file found for server pid $p)"
            continue
        }
        foreach f $files {dump_crash_report $f $p}
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
    fconfigure $::test_server_fd -translation binary

    # A SIGUSR1 from the test server means "you look stuck -- say where".
    # "signal error" raises a Tcl error at the next point Tcl checks for
    # signals, which unwinds through the handler at the bottom of this file and
    # reports $::errorInfo, a Tcl stack trace of wherever we hung. That covers a
    # blocking read (the syscall is interrupted rather than retried), a long
    # "after" and a polling loop alike; a plain "signal trap" only covers the
    # first. It also keeps the signal from simply killing us. Tclx is optional:
    # without it a timeout just reports no client stack trace.
    if {![catch {package require Tclx; signal error SIGUSR1}]} {
        # Only now may the test server signal us: without the handler,
        # SIGUSR1 just kills a client that could still be about to report.
        send_data_packet $::test_server_fd sigusr1-trace 1
    }

    send_data_packet $::test_server_fd ready [pid]
    while 1 {
        set bytes [gets $::test_server_fd]
        set payload [encoding convertfrom utf-8 [read $::test_server_fd $bytes]]
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
    # Convert to UTF-8 bytes before sending so that:
    # 1. The byte count is accurate (Tcl 9.0 string length returns character
    #    count, which differs from byte count for non-ASCII Unicode chars).
    # 2. Characters above U+00FF (not representable in the channel's iso8859-1
    #    encoding) are safely transmitted as multi-byte UTF-8 sequences.
    set payload_bytes [encoding convertto utf-8 $payload]
    puts $fd [string length $payload_bytes]
    puts -nonewline $fd $payload_bytes
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
        "--cluster          Run only the cluster tests (tests/unit/cluster)."
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
        "--debug-defrag     Indicate the test is running against server compiled with DEBUG_DEFRAG option"
        "--help             Print this help screen."
    } "\n"]
}

# parse arguments
for {set j 0} {$j < [llength $argv]} {incr j} {
    set opt [lindex $argv $j]
    set arg [lindex $argv [expr $j+1]]
    if {$opt eq {--cluster}} {
        set ::all_tests $::cluster_all_tests
    } elseif {$opt eq {--tags}} {
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
    } elseif {$opt eq {--debug-defrag}} {
        set ::debug_defrag 1
    } elseif {$opt eq {--compression}} {
        set ::compression 1
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
