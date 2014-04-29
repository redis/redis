# Cluster-specific test functions.
#
# Copyright (C) 2014 Salvatore Sanfilippo antirez@gmail.com
# This softare is released under the BSD License. See the COPYING file for
# more information.

# Returns a parsed CLUSTER NODES output as a list of dictionaries.
proc get_cluster_nodes id {
    set lines [split [R $id cluster nodes] "\r\n"]
    set nodes {}
    foreach l $lines {
        set l [string trim $l]
        if {$l eq {}} continue
        set args [split $l]
        set node [dict create \
            id [lindex $args 0] \
            addr [lindex $args 1] \
            flags [split [lindex $args 2] ,] \
            slaveof [lindex $args 3] \
            ping_sent [lindex $args 4] \
            pong_recv [lindex $args 5] \
            config_epoch [lindex $args 6] \
            linkstate [lindex $args 7] \
            slots [lrange $args 8 -1] \
        ]
        lappend nodes $node
    }
    return $nodes
}

# Test node for flag.
proc has_flag {node flag} {
    expr {[lsearch -exact [dict get $node flags] $flag] != -1}
}

# Returns the parsed myself node entry as a dictionary.
proc get_myself id {
    set nodes [get_cluster_nodes $id]
    foreach n $nodes {
        if {[has_flag $n myself]} {return $n}
    }
    return {}
}

