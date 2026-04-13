#
# Copyright (c) 2009-Present, Redis Ltd.
# All rights reserved.
#
# Copyright (c) 2024-present, Valkey contributors.
# All rights reserved.
#
# Licensed under your choice of (a) the Redis Source Available License 2.0
# (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
# GNU Affero General Public License v3 (AGPLv3).
#
# Portions of this file are available under BSD3 terms; see REDISCONTRIBUTIONS for more information.
#

source tests/support/redis.tcl

set ::tlsdir "tests/tls"

# Continuously sends SET commands to the server. If key is omitted, a random key
# is used for every SET command. The value is always random.
# cluster_load (default 0): when 1, MOVED/ASK replies are tolerated while
# draining pipelined responses.
proc gen_write_load {host port seconds tls {key ""} {size 0} {sleep 0} {cluster_load 0}} {
    set start_time [clock seconds]
    set r [redis $host $port 1 $tls]
    $r client setname LOAD_HANDLER
    $r read
    catch {
        $r select 9
        $r read
    } ;# select 9 will fail in cluster mode

    # fixed size value
    if {$size != 0} {
        set value [string repeat "x" $size]
    }

    set count 0
    while 1 {
        if {$size == 0} {
            set value [expr rand()]
        }

        if {$key == ""} {
            $r set [expr rand()] $value
        } else {
            $r set $key $value
        }

        incr count
        if {$count % 500 == 0} {
            for {set i 0} {$i < 500} {incr i} {
                if {$cluster_load == 1} {
                    if {[catch {$r read} err]} {
                        if {[string match {MOVED*} $err] || [string match {ASK*} $err]} {
                            continue
                        }
                        error $err
                    }
                } else {
                    $r read
                }
            }
            set count 0
        }

        if {[clock seconds]-$start_time > $seconds} {
            break
        }
        if {$sleep ne 0} {
            after $sleep
        }
    }

    # Read remaining replies
    for {set i 0} {$i < $count} {incr i} {
        if {$cluster_load == 1} {
            if {[catch {$r read} err]} {
                if {[string match {MOVED*} $err] || [string match {ASK*} $err]} {
                    continue
                }
                error $err
            }
        } else {
            $r read
        }
    }
    exit 0
}

set cluster_load 0
if {[llength $argv] > 7} {
    set cluster_load [lindex $argv 7]
}
gen_write_load [lindex $argv 0] [lindex $argv 1] [lindex $argv 2] [lindex $argv 3] [lindex $argv 4] [lindex $argv 5] [lindex $argv 6] $cluster_load
