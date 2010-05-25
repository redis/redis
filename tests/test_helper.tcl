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

proc execute_tests name {
    set cur $::testnum
    source "tests/$name.tcl"
}

# Setup a list to hold a stack of server configs. When calls to start_server
# are nested, use "srv 0 pid" to get the pid of the inner server. To access
# outer servers, use "srv -1 pid" etcetera.
set ::servers {}
proc srv {level property} {
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

proc main {} {
    cleanup
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
    execute_tests "integration/replication"
    execute_tests "integration/aof"

    # run tests with VM enabled
    set ::global_overrides [list [list vm-enabled yes]]
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
    
    puts "\n[expr $::passed+$::failed] tests, $::passed passed, $::failed failed"
    if {$::failed > 0} {
        puts "\n*** WARNING!!! $::failed FAILED TESTS ***\n"
    }

    cleanup
}

main
