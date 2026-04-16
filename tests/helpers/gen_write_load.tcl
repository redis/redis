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
# ignore_error_reply (default 0): when non-zero, MOVED/ASK replies are tolerated
# while draining pipelined responses (periodic 500-reply batches and final drain).
proc gen_write_load {host port seconds tls {key ""} {size 0} {sleep 0} {ignore_error_reply 0}} {
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
                # Capture opts to preserve original errorInfo/errorCode on re-raise.
                if {[catch {$r read} err opts]} {
                    if {$ignore_error_reply && ([string match {MOVED*} $err] || [string match {ASK*} $err])} {
                        continue
                    }
                    return -options $opts $err
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
        if {[catch {$r read} err opts]} {
            if {$ignore_error_reply && ([string match {MOVED*} $err] || [string match {ASK*} $err])} {
                continue
            }
            return -options $opts $err
        }
    }
    exit 0
}

gen_write_load [lindex $argv 0] [lindex $argv 1] [lindex $argv 2] [lindex $argv 3] [lindex $argv 4] [lindex $argv 5] [lindex $argv 6] [lindex $argv 7]
