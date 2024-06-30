# Cluster test suite.
#
# Copyright (C) 2014-Present, Redis Ltd.
# All Rights reserved.
#
# Licensed under your choice of the Redis Source Available License 2.0
# (RSALv2) or the Server Side Public License v1 (SSPLv1).

cd tests/cluster
source cluster.tcl
source ../instances.tcl
source ../../support/cluster.tcl ; # Redis Cluster client.

set ::instances_count 20 ; # How many instances we use at max.
set ::tlsdir "../../tls"

proc main {} {
    parse_options
    spawn_instance redis $::redis_base_port $::instances_count {
        "cluster-enabled yes"
        "appendonly yes"
        "enable-protected-configs yes"
        "enable-debug-command yes"
        "save ''"
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
