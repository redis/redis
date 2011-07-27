# Redis test suite. Copyright (C) 2009 Salvatore Sanfilippo antirez@gmail.com
# This softare is released under the BSD License. See the COPYING file for
# more information.

set tcl_precision 17
source tests/support/redis.tcl
source tests/support/server.tcl
source tests/support/tmpfile.tcl
source tests/support/test.tcl
source tests/support/util.tcl

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

proc execute_tests name {
    set path "tests/$name.tcl"
    set ::curfile $path
    source $path
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
    catch {exec rm -rf {*}[glob tests/tmp/redis.conf.*]}
    catch {exec rm -rf {*}[glob tests/tmp/server.*]}
}

proc execute_everything {} {
    execute_tests "unit/printver"
    execute_tests "unit/auth"
    execute_tests "unit/protocol"
    execute_tests "unit/basic"
    execute_tests "unit/type/list"
    execute_tests "unit/type/set"
    execute_tests "unit/type/zset"
    execute_tests "unit/type/hash"
    execute_tests "unit/sort"
    execute_tests "unit/expire"
    execute_tests "unit/other"
    execute_tests "unit/cas"
    execute_tests "unit/quit"
    execute_tests "integration/replication"
    execute_tests "integration/aof"
#    execute_tests "integration/redis-cli"
    execute_tests "unit/pubsub"
    execute_tests "unit/slowlog"

    # run tests with VM enabled
    set ::global_overrides {vm-enabled yes really-use-vm yes}
    execute_tests "unit/protocol"
    execute_tests "unit/basic"
    execute_tests "unit/type/list"
    execute_tests "unit/type/set"
    execute_tests "unit/type/zset"
    execute_tests "unit/type/hash"
    execute_tests "unit/sort"
    execute_tests "unit/expire"
    execute_tests "unit/other"
    execute_tests "unit/cas"
}

proc main {} {
    cleanup

    if {[string length $::file] > 0} {
        foreach {file} [split $::file ,] {
            execute_tests $file
        }
    } else {
        execute_everything
    }

    cleanup
    puts "\n[expr $::num_tests] tests, $::num_passed passed, $::num_failed failed\n"
    if {$::num_failed > 0} {
        set curheader ""
        puts "Failures:"
        foreach {test} $::tests_failed {
            set header [lindex $test 0]
            append header " ("
            append header [join [lindex $test 1] ","]
            append header ")"

            if {$curheader ne $header} {
                set curheader $header
                puts "\n$curheader:"
            }

            set name [lindex $test 2]
            set msg [lindex $test 3]
            puts "- $name: $msg"
        }

        puts ""
        exit 1
    }
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
    } else {
        puts "Wrong argument: $opt"
        exit 1
    }
}

if {[catch { main } err]} {
    if {[string length $err] > 0} {
        # only display error when not generated by the test suite
        if {$err ne "exception"} {
            puts $::errorInfo
        }
        exit 1
    }
}
