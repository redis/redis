# Cluster test suite. Copyright (C) 2014 Salvatore Sanfilippo antirez@gmail.com
# This softare is released under the BSD License. See the COPYING file for
# more information.

cd tests/cluster
source cluster.tcl
source ../instances.tcl

set ::instances_count 5 ; # How many instances we use at max.

proc main {} {
    parse_options
    spawn_instance redis $::redis_base_port $::instances_count {
        "cluster-enabled yes"
        "appendonly yes"
    }
    run_tests
    cleanup
}

if {[catch main e]} {
    puts $::errorInfo
    cleanup
}
