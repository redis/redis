# Sentinel test suite. Copyright (C) 2014 Salvatore Sanfilippo antirez@gmail.com
# This software is released under the BSD License. See the COPYING file for
# more information.

cd tests/sentinel
source ../instances.tcl

set ::instances_count 5 ; # How many instances we use at max.
set ::tlsdir "../../tls"

proc main {} {
    parse_options
    spawn_instance sentinel $::sentinel_base_port $::instances_count
    spawn_instance redis $::redis_base_port $::instances_count
    run_tests
    cleanup
    end_tests
}

if {[catch main e]} {
    puts $::errorInfo
    cleanup
    exit 1
}
