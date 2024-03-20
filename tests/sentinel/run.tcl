# Sentinel test suite.
#
# Copyright (C) 2014-Present, Redis Ltd.
# All Rights reserved.
#
# Licensed under your choice of the Redis Source Available License 2.0
# (RSALv2) or the Server Side Public License v1 (SSPLv1).

cd tests/sentinel
source ../instances.tcl

set ::instances_count 5 ; # How many instances we use at max.
set ::tlsdir "../../tls"

proc main {} {
    parse_options
    if {$::leaked_fds_file != ""} {
        set ::env(LEAKED_FDS_FILE) $::leaked_fds_file
    }
    spawn_instance sentinel $::sentinel_base_port $::instances_count {
        "sentinel deny-scripts-reconfig no"
        "enable-protected-configs yes"
        "enable-debug-command yes"
    } "../tests/includes/sentinel.conf"

    spawn_instance redis $::redis_base_port $::instances_count {
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
    cleanup
    exit 1
}
