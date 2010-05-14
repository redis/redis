# Redis test suite. Copyright (C) 2009 Salvatore Sanfilippo antirez@gmail.com
# This softare is released under the BSD License. See the COPYING file for
# more information.

set tcl_precision 17
source test/support/redis.tcl
source test/support/server.tcl
source test/support/tmpfile.tcl
source test/support/test.tcl
source test/support/util.tcl

set ::host 127.0.0.1
set ::port 6379
set ::traceleaks 0

proc execute_tests name {
    set cur $::testnum
    source "test/$name.tcl"
}

# setup a list to hold a stack of clients. the proc "r" provides easy
# access to the client at the top of the stack
set ::clients {}
proc r {args} {
    set client [lindex $::clients end]
    $client {*}$args
}

proc main {} {
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
    
    puts "\n[expr $::passed+$::failed] tests, $::passed passed, $::failed failed"
    if {$::failed > 0} {
        puts "\n*** WARNING!!! $::failed FAILED TESTS ***\n"
    }
    
    # clean up tmp
    exec rm -rf test/tmp/*
}

main
