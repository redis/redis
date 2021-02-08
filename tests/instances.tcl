# Multi-instance test framework.
# This is used in order to test Sentinel and Redis Cluster, and provides
# basic capabilities for spawning and handling N parallel Redis / Sentinel
# instances.
#
# Copyright (C) 2014 Salvatore Sanfilippo antirez@gmail.com
# This software is released under the BSD License. See the COPYING file for
# more information.

package require Tcl 8.5

set tcl_precision 17
source ../support/redis.tcl
source ../support/util.tcl
source ../support/server.tcl
source ../support/test.tcl

set ::verbose 0
set ::valgrind 0
set ::tls 0
set ::pause_on_error 0
set ::dont_clean 0
set ::simulate_error 0
set ::failed 0
set ::sentinel_instances {}
set ::redis_instances {}
set ::global_config {}
set ::sentinel_base_port 20000
set ::redis_base_port 30000
set ::redis_port_count 1024
set ::host "127.0.0.1"
set ::leaked_fds_file [file normalize "tmp/leaked_fds.txt"]
set ::pids {} ; # We kill everything at exit
set ::dirs {} ; # We remove all the temp dirs at exit
set ::run_matching {} ; # If non empty, only tests matching pattern are run.

if {[catch {cd tmp}]} {
    puts "tmp directory not found."
    puts "Please run this test from the Redis source root."
    exit 1
}

# Execute the specified instance of the server specified by 'type', using
# the provided configuration file. Returns the PID of the process.
proc exec_instance {type dirname cfgfile} {
    if {$type eq "redis"} {
        set prgname redis-server
    } elseif {$type eq "sentinel"} {
        set prgname redis-sentinel
    } else {
        error "Unknown instance type."
    }

    set errfile [file join $dirname err.txt]
    if {$::valgrind} {
        set pid [exec valgrind --track-origins=yes --suppressions=../../../src/valgrind.sup --show-reachable=no --show-possibly-lost=no --leak-check=full ../../../src/${prgname} $cfgfile 2>> $errfile &]
    } else {
        set pid [exec ../../../src/${prgname} $cfgfile 2>> $errfile &]
    }
    return $pid
}

# Spawn a redis or sentinel instance, depending on 'type'.
proc spawn_instance {type base_port count {conf {}} {base_conf_file ""}} {
    for {set j 0} {$j < $count} {incr j} {
        set port [find_available_port $base_port $::redis_port_count]
        # Create a directory for this instance.
        set dirname "${type}_${j}"
        lappend ::dirs $dirname
        catch {exec rm -rf $dirname}
        file mkdir $dirname

        # Write the instance config file.
        set cfgfile [file join $dirname $type.conf]
        if {$base_conf_file ne ""} {
            file copy -- $base_conf_file $cfgfile
            set cfg [open $cfgfile a+]
        } else {
            set cfg [open $cfgfile w]
        }

        if {$::tls} {
            puts $cfg "tls-port $port"
            puts $cfg "tls-replication yes"
            puts $cfg "tls-cluster yes"
            puts $cfg "port 0"
            puts $cfg [format "tls-cert-file %s/../../tls/server.crt" [pwd]]
            puts $cfg [format "tls-key-file %s/../../tls/server.key" [pwd]]
            puts $cfg [format "tls-client-cert-file %s/../../tls/client.crt" [pwd]]
            puts $cfg [format "tls-client-key-file %s/../../tls/client.key" [pwd]]
            puts $cfg [format "tls-dh-params-file %s/../../tls/redis.dh" [pwd]]
            puts $cfg [format "tls-ca-cert-file %s/../../tls/ca.crt" [pwd]]
            puts $cfg "loglevel debug"
        } else {
            puts $cfg "port $port"
        }
        puts $cfg "dir ./$dirname"
        puts $cfg "logfile log.txt"
        # Add additional config files
        foreach directive $conf {
            puts $cfg $directive
        }
        dict for {name val} $::global_config {
            puts $cfg "$name $val"
        }
        close $cfg

        # Finally exec it and remember the pid for later cleanup.
        set retry 100
        while {$retry} {
            set pid [exec_instance $type $dirname $cfgfile]

            # Check availability
            if {[server_is_up 127.0.0.1 $port 100] == 0} {
                puts "Starting $type #$j at port $port failed, try another"
                incr retry -1
                set port [find_available_port $base_port $::redis_port_count]
                set cfg [open $cfgfile a+]
                if {$::tls} {
                    puts $cfg "tls-port $port"
                } else {
                    puts $cfg "port $port"
                }
                close $cfg
            } else {
                puts "Starting $type #$j at port $port"
                lappend ::pids $pid
                break
            }
        }

        # Check availability finally
        if {[server_is_up $::host $port 100] == 0} {
            set logfile [file join $dirname log.txt]
            puts [exec tail $logfile]
            abort_sentinel_test "Problems starting $type #$j: ping timeout, maybe server start failed, check $logfile"
        }

        # Push the instance into the right list
        set link [redis $::host $port 0 $::tls]
        $link reconnect 1
        lappend ::${type}_instances [list \
            pid $pid \
            host $::host \
            port $port \
            link $link \
        ]
    }
}

proc log_crashes {} {
    set start_pattern {*REDIS BUG REPORT START*}
    set logs [glob */log.txt]
    foreach log $logs {
        set fd [open $log]
        set found 0
        while {[gets $fd line] >= 0} {
            if {[string match $start_pattern $line]} {
                puts "\n*** Crash report found in $log ***"
                set found 1
            }
            if {$found} {
                puts $line
                incr ::failed
            }
        }
    }

    set logs [glob */err.txt]
    foreach log $logs {
        set res [find_valgrind_errors $log true]
        if {$res != ""} {
            puts $res
            incr ::failed
        }
    }
}

proc is_alive pid {
    if {[catch {exec ps -p $pid} err]} {
        return 0
    } else {
        return 1
    }
}

proc stop_instance pid {
    catch {exec kill $pid}
    if {$::valgrind} {
        set max_wait 60000
    } else {
        set max_wait 10000
    }
    while {[is_alive $pid]} {
        incr wait 10

        if {$wait >= $max_wait} {
            puts "Forcing process $pid to exit..."
            catch {exec kill -KILL $pid}
        } elseif {$wait % 1000 == 0} {
            puts "Waiting for process $pid to exit..."
        }
        after 10
    }
}

proc cleanup {} {
    puts "Cleaning up..."
    foreach pid $::pids {
        puts "killing stale instance $pid"
        stop_instance $pid
    }
    log_crashes
    if {$::dont_clean} {
        return
    }
    foreach dir $::dirs {
        catch {exec rm -rf $dir}
    }
}

proc abort_sentinel_test msg {
    incr ::failed
    puts "WARNING: Aborting the test."
    puts ">>>>>>>> $msg"
    if {$::pause_on_error} pause_on_error
    cleanup
    exit 1
}

proc parse_options {} {
    for {set j 0} {$j < [llength $::argv]} {incr j} {
        set opt [lindex $::argv $j]
        set val [lindex $::argv [expr $j+1]]
        if {$opt eq "--single"} {
            incr j
            set ::run_matching "*${val}*"
        } elseif {$opt eq "--pause-on-error"} {
            set ::pause_on_error 1
        } elseif {$opt eq {--dont-clean}} {
            set ::dont_clean 1
        } elseif {$opt eq "--fail"} {
            set ::simulate_error 1
        } elseif {$opt eq {--valgrind}} {
            set ::valgrind 1
        } elseif {$opt eq {--host}} {
            incr j
            set ::host ${val}
        } elseif {$opt eq {--tls}} {
            package require tls 1.6
            ::tls::init \
                -cafile "$::tlsdir/ca.crt" \
                -certfile "$::tlsdir/client.crt" \
                -keyfile "$::tlsdir/client.key"
            set ::tls 1
        } elseif {$opt eq {--config}} {
            set val2 [lindex $::argv [expr $j+2]]
            dict set ::global_config $val $val2
            incr j 2
        } elseif {$opt eq "--help"} {
            puts "--single <pattern>      Only runs tests specified by pattern."
            puts "--dont-clean            Keep log files on exit."
            puts "--pause-on-error        Pause for manual inspection on error."
            puts "--fail                  Simulate a test failure."
            puts "--valgrind              Run with valgrind."
            puts "--tls                   Run tests in TLS mode."
            puts "--host <host>           Use hostname instead of 127.0.0.1."
            puts "--config <k> <v>        Extra config argument(s)."
            puts "--help                  Shows this help."
            exit 0
        } else {
            puts "Unknown option $opt"
            exit 1
        }
    }
}

# If --pause-on-error option was passed at startup this function is called
# on error in order to give the developer a chance to understand more about
# the error condition while the instances are still running.
proc pause_on_error {} {
    puts ""
    puts [colorstr yellow "*** Please inspect the error now ***"]
    puts "\nType \"continue\" to resume the test, \"help\" for help screen.\n"
    while 1 {
        puts -nonewline "> "
        flush stdout
        set line [gets stdin]
        set argv [split $line " "]
        set cmd [lindex $argv 0]
        if {$cmd eq {continue}} {
            break
        } elseif {$cmd eq {show-redis-logs}} {
            set count 10
            if {[lindex $argv 1] ne {}} {set count [lindex $argv 1]}
            foreach_redis_id id {
                puts "=== REDIS $id ===="
                puts [exec tail -$count redis_$id/log.txt]
                puts "---------------------\n"
            }
        } elseif {$cmd eq {show-sentinel-logs}} {
            set count 10
            if {[lindex $argv 1] ne {}} {set count [lindex $argv 1]}
            foreach_sentinel_id id {
                puts "=== SENTINEL $id ===="
                puts [exec tail -$count sentinel_$id/log.txt]
                puts "---------------------\n"
            }
        } elseif {$cmd eq {ls}} {
            foreach_redis_id id {
                puts -nonewline "Redis $id"
                set errcode [catch {
                    set str {}
                    append str "@[RI $id tcp_port]: "
                    append str "[RI $id role] "
                    if {[RI $id role] eq {slave}} {
                        append str "[RI $id master_host]:[RI $id master_port]"
                    }
                    set str
                } retval]
                if {$errcode} {
                    puts " -- $retval"
                } else {
                    puts $retval
                }
            }
            foreach_sentinel_id id {
                puts -nonewline "Sentinel $id"
                set errcode [catch {
                    set str {}
                    append str "@[SI $id tcp_port]: "
                    append str "[join [S $id sentinel get-master-addr-by-name mymaster]]"
                    set str
                } retval]
                if {$errcode} {
                    puts " -- $retval"
                } else {
                    puts $retval
                }
            }
        } elseif {$cmd eq {help}} {
            puts "ls                     List Sentinel and Redis instances."
            puts "show-sentinel-logs \[N\] Show latest N lines of logs."
            puts "show-redis-logs \[N\]    Show latest N lines of logs."
            puts "S <id> cmd ... arg     Call command in Sentinel <id>."
            puts "R <id> cmd ... arg     Call command in Redis <id>."
            puts "SI <id> <field>        Show Sentinel <id> INFO <field>."
            puts "RI <id> <field>        Show Redis <id> INFO <field>."
            puts "continue               Resume test."
        } else {
            set errcode [catch {eval $line} retval]
            if {$retval ne {}} {puts "$retval"}
        }
    }
}

# We redefine 'test' as for Sentinel we don't use the server-client
# architecture for the test, everything is sequential.
proc test {descr code} {
    set ts [clock format [clock seconds] -format %H:%M:%S]
    puts -nonewline "$ts> $descr: "
    flush stdout

    if {[catch {set retval [uplevel 1 $code]} error]} {
        incr ::failed
        if {[string match "assertion:*" $error]} {
            set msg [string range $error 10 end]
            puts [colorstr red $msg]
            if {$::pause_on_error} pause_on_error
            puts "(Jumping to next unit after error)"
            return -code continue
        } else {
            # Re-raise, let handler up the stack take care of this.
            error $error $::errorInfo
        }
    } else {
        puts [colorstr green OK]
    }
}

# Check memory leaks when running on OSX using the "leaks" utility.
proc check_leaks instance_types {
    if {[string match {*Darwin*} [exec uname -a]]} {
        puts -nonewline "Testing for memory leaks..."; flush stdout
        foreach type $instance_types {
            foreach_instance_id [set ::${type}_instances] id {
                if {[instance_is_killed $type $id]} continue
                set pid [get_instance_attrib $type $id pid]
                set output {0 leaks}
                catch {exec leaks $pid} output
                if {[string match {*process does not exist*} $output] ||
                    [string match {*cannot examine*} $output]} {
                    # In a few tests we kill the server process.
                    set output "0 leaks"
                } else {
                    puts -nonewline "$type/$pid "
                    flush stdout
                }
                if {![string match {*0 leaks*} $output]} {
                    puts [colorstr red "=== MEMORY LEAK DETECTED ==="]
                    puts "Instance type $type, ID $id:"
                    puts $output
                    puts "==="
                    incr ::failed
                }
            }
        }
        puts ""
    }
}

# Execute all the units inside the 'tests' directory.
proc run_tests {} {
    set tests [lsort [glob ../tests/*]]
    foreach test $tests {
        # Remove leaked_fds file before starting
        if {$::leaked_fds_file != "" && [file exists $::leaked_fds_file]} {
            file delete $::leaked_fds_file
        }

        if {$::run_matching ne {} && [string match $::run_matching $test] == 0} {
            continue
        }
        if {[file isdirectory $test]} continue
        puts [colorstr yellow "Testing unit: [lindex [file split $test] end]"]
        source $test
        check_leaks {redis sentinel}

        # Check if a leaked fds file was created and abort the test.
        if {$::leaked_fds_file != "" && [file exists $::leaked_fds_file]} {
            puts [colorstr red "ERROR: Sentinel has leaked fds to scripts:"]
            puts [exec cat $::leaked_fds_file]
            puts "----"
            incr ::failed
        }
    }
}

# Print a message and exists with 0 / 1 according to zero or more failures.
proc end_tests {} {
    if {$::failed == 0 } {
        puts "GOOD! No errors."
        exit 0
    } else {
        puts "WARNING $::failed test(s) failed."
        exit 1
    }
}

# The "S" command is used to interact with the N-th Sentinel.
# The general form is:
#
# S <sentinel-id> command arg arg arg ...
#
# Example to ping the Sentinel 0 (first instance): S 0 PING
proc S {n args} {
    set s [lindex $::sentinel_instances $n]
    [dict get $s link] {*}$args
}

# Returns a Redis instance by index.
# Example:
#     [Rn 0] info
proc Rn {n} {
    return [dict get [lindex $::redis_instances $n] link]
}

# Like R but to chat with Redis instances.
proc R {n args} {
    [Rn $n] {*}$args
}

proc get_info_field {info field} {
    set fl [string length $field]
    append field :
    foreach line [split $info "\n"] {
        set line [string trim $line "\r\n "]
        if {[string range $line 0 $fl] eq $field} {
            return [string range $line [expr {$fl+1}] end]
        }
    }
    return {}
}

proc SI {n field} {
    get_info_field [S $n info] $field
}

proc RI {n field} {
    get_info_field [R $n info] $field
}

# Iterate over IDs of sentinel or redis instances.
proc foreach_instance_id {instances idvar code} {
    upvar 1 $idvar id
    for {set id 0} {$id < [llength $instances]} {incr id} {
        set errcode [catch {uplevel 1 $code} result]
        if {$errcode == 1} {
            error $result $::errorInfo $::errorCode
        } elseif {$errcode == 4} {
            continue
        } elseif {$errcode == 3} {
            break
        } elseif {$errcode != 0} {
            return -code $errcode $result
        }
    }
}

proc foreach_sentinel_id {idvar code} {
    set errcode [catch {uplevel 1 [list foreach_instance_id $::sentinel_instances $idvar $code]} result]
    return -code $errcode $result
}

proc foreach_redis_id {idvar code} {
    set errcode [catch {uplevel 1 [list foreach_instance_id $::redis_instances $idvar $code]} result]
    return -code $errcode $result
}

# Get the specific attribute of the specified instance type, id.
proc get_instance_attrib {type id attrib} {
    dict get [lindex [set ::${type}_instances] $id] $attrib
}

# Set the specific attribute of the specified instance type, id.
proc set_instance_attrib {type id attrib newval} {
    set d [lindex [set ::${type}_instances] $id]
    dict set d $attrib $newval
    lset ::${type}_instances $id $d
}

# Create a master-slave cluster of the given number of total instances.
# The first instance "0" is the master, all others are configured as
# slaves.
proc create_redis_master_slave_cluster n {
    foreach_redis_id id {
        if {$id == 0} {
            # Our master.
            R $id slaveof no one
            R $id flushall
        } elseif {$id < $n} {
            R $id slaveof [get_instance_attrib redis 0 host] \
                          [get_instance_attrib redis 0 port]
        } else {
            # Instances not part of the cluster.
            R $id slaveof no one
        }
    }
    # Wait for all the slaves to sync.
    wait_for_condition 1000 50 {
        [RI 0 connected_slaves] == ($n-1)
    } else {
        fail "Unable to create a master-slaves cluster."
    }
}

proc get_instance_id_by_port {type port} {
    foreach_${type}_id id {
        if {[get_instance_attrib $type $id port] == $port} {
            return $id
        }
    }
    fail "Instance $type port $port not found."
}

# Kill an instance of the specified type/id with SIGKILL.
# This function will mark the instance PID as -1 to remember that this instance
# is no longer running and will remove its PID from the list of pids that
# we kill at cleanup.
#
# The instance can be restarted with restart-instance.
proc kill_instance {type id} {
    set pid [get_instance_attrib $type $id pid]
    set port [get_instance_attrib $type $id port]

    if {$pid == -1} {
        error "You tried to kill $type $id twice."
    }

    stop_instance $pid
    set_instance_attrib $type $id pid -1
    set_instance_attrib $type $id link you_tried_to_talk_with_killed_instance

    # Remove the PID from the list of pids to kill at exit.
    set ::pids [lsearch -all -inline -not -exact $::pids $pid]

    # Wait for the port it was using to be available again, so that's not
    # an issue to start a new server ASAP with the same port.
    set retry 100
    while {[incr retry -1]} {
        set port_is_free [catch {set s [socket 127.0.0.1 $port]}]
        if {$port_is_free} break
        catch {close $s}
        after 100
    }
    if {$retry == 0} {
        error "Port $port does not return available after killing instance."
    }
}

# Return true of the instance of the specified type/id is killed.
proc instance_is_killed {type id} {
    set pid [get_instance_attrib $type $id pid]
    expr {$pid == -1}
}

# Restart an instance previously killed by kill_instance
proc restart_instance {type id} {
    set dirname "${type}_${id}"
    set cfgfile [file join $dirname $type.conf]
    set port [get_instance_attrib $type $id port]

    # Execute the instance with its old setup and append the new pid
    # file for cleanup.
    set pid [exec_instance $type $dirname $cfgfile]
    set_instance_attrib $type $id pid $pid
    lappend ::pids $pid

    # Check that the instance is running
    if {[server_is_up 127.0.0.1 $port 100] == 0} {
        set logfile [file join $dirname log.txt]
        puts [exec tail $logfile]
        abort_sentinel_test "Problems starting $type #$id: ping timeout, maybe server start failed, check $logfile"
    }

    # Connect with it with a fresh link
    set link [redis 127.0.0.1 $port 0 $::tls]
    $link reconnect 1
    set_instance_attrib $type $id link $link

    # Make sure the instance is not loading the dataset when this
    # function returns.
    while 1 {
        catch {[$link ping]} retval
        if {[string match {*LOADING*} $retval]} {
            after 100
            continue
        } else {
            break
        }
    }
}

proc redis_deferring_client {type id} {
    set port [get_instance_attrib $type $id port]
    set host [get_instance_attrib $type $id host]
    set client [redis $host $port 1 $::tls]
    return $client
}

proc redis_client {type id} {
    set port [get_instance_attrib $type $id port]
    set host [get_instance_attrib $type $id host]
    set client [redis $host $port 0 $::tls]
    return $client
}
