# Cluster test suite. Copyright (C) 2014 Salvatore Sanfilippo antirez@gmail.com
# This software is released under the BSD License. See the COPYING file for
# more information.

cd tests/cluster
source cluster.tcl
source ../instances.tcl
source ../../support/cluster.tcl ; # Redis Cluster client.

set ::instances_count 20 ; # How many instances we use at max.

proc main {} {
    parse_options
    spawn_instance redis $::redis_base_port $::instances_count {
        "cluster-enabled yes"
        "appendonly yes"
    }
    run_tests
    cleanup
    end_tests
}

if {[catch main e]} {
    puts $::errorInfo
    if {$::pause_on_error} pause_on_error
    cleanup
    exit 1
}
