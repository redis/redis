# Tcl redis cluster client as a wrapper of redis.rb.
# Copyright (C) 2014 Salvatore Sanfilippo
# Released under the BSD license like Redis itself
#
# Example usage:
#
# set c [redis_cluster 127.0.0.1 6379 127.0.0.1 6380]
# $c set foo
# $c get foo
# $c close

package require Tcl 8.5
package provide redis_cluster 0.1

namespace eval redis_cluster {}
set ::redis_cluster::id 0
array set ::redis_cluster::startup_nodes {}
array set ::redis_cluster::nodes {}
array set ::redis_cluster::slots {}

# List of "plain" commands, which are commands where the sole key is always
# the first argument.
set ::redis_cluster::plain_commands {
    get set setnx setex psetex append strlen exists setbit getbit
    setrange getrange substr incr decr rpush lpush rpushx lpushx
    linsert rpop lpop brpop llen lindex lset lrange ltrim lrem
    sadd srem sismember scard spop srandmember smembers sscan zadd
    zincrby zrem zremrangebyscore zremrangebyrank zremrangebylex zrange
    zrangebyscore zrevrangebyscore zrangebylex zrevrangebylex zcount
    zlexcount zrevrange zcard zscore zrank zrevrank zscan hset hsetnx
    hget hmset hmget hincrby hincrbyfloat hdel hlen hkeys hvals
    hgetall hexists hscan incrby decrby incrbyfloat getset move
    expire expireat pexpire pexpireat type ttl pttl persist restore
    dump bitcount bitpos pfadd pfcount
}

proc redis_cluster {nodes} {
    set id [incr ::redis_cluster::id]
    set ::redis_cluster::startup_nodes($id) $nodes
    set ::redis_cluster::nodes($id) {}
    set ::redis_cluster::slots($id) {}
    set handle [interp alias {} ::redis_cluster::instance$id {} ::redis_cluster::__dispatch__ $id]
    $handle refresh_nodes_map
    return $handle
}

# Totally reset the slots / nodes state for the client, calls
# CLUSTER NODES in the first startup node available, populates the
# list of nodes ::redis_cluster::nodes($id) with an hash mapping node
# ip:port to a representation of the node (another hash), and finally
# maps ::redis_cluster::slots($id) with an hash mapping slot numbers
# to node IDs.
#
# This function is called when a new Redis Cluster client is initialized
# and every time we get a -MOVED redirection error.
proc ::redis_cluster::__method__refresh_nodes_map {id} {
    # Contact the first responding startup node.
    set idx 0; # Index of the node that will respond.
    set errmsg {}
    foreach start_node $::redis_cluster::startup_nodes($id) {
        set ip_port [lindex [split $start_node @] 0]
        lassign [split $ip_port :] start_host start_port
        if {[catch {
            set r {}
            set r [redis $start_host $start_port 0 $::tls]
            set nodes_descr [$r cluster nodes]
            $r close
        } e]} {
            if {$r ne {}} {catch {$r close}}
            incr idx
            if {[string length $errmsg] < 200} {
                append errmsg " $ip_port: $e"
            }
            continue ; # Try next.
        } else {
            break; # Good node found.
        }
    }

    if {$idx == [llength $::redis_cluster::startup_nodes($id)]} {
        error "No good startup node found. $errmsg"
    }

    # Put the node that responded as first in the list if it is not
    # already the first.
    if {$idx != 0} {
        set l $::redis_cluster::startup_nodes($id)
        set left [lrange $l 0 [expr {$idx-1}]]
        set right [lrange $l [expr {$idx+1}] end]
        set l [concat [lindex $l $idx] $left $right]
        set ::redis_cluster::startup_nodes($id) $l
    }

    # Parse CLUSTER NODES output to populate the nodes description.
    set nodes {} ; # addr -> node description hash.
    foreach line [split $nodes_descr "\n"] {
        set line [string trim $line]
        if {$line eq {}} continue
        set args [split $line " "]
        lassign $args nodeid addr flags slaveof pingsent pongrecv configepoch linkstate
        set slots [lrange $args 8 end]
        set addr [lindex [split $addr @] 0]
        if {$addr eq {:0}} {
            set addr $start_host:$start_port
        }
        lassign [split $addr :] host port

        # Connect to the node
        set link {}
        catch {set link [redis $host $port 0 $::tls]}

        # Build this node description as an hash.
        set node [dict create \
            id $nodeid \
            addr $addr \
            host $host \
            port $port \
            flags $flags \
            slaveof $slaveof \
            slots $slots \
            link $link \
        ]
        dict set nodes $addr $node
        lappend ::redis_cluster::startup_nodes($id) $addr
    }

    # Close all the existing links in the old nodes map, and set the new
    # map as current.
    foreach n $::redis_cluster::nodes($id) {
        catch {
            [dict get $n link] close
        }
    }
    set ::redis_cluster::nodes($id) $nodes

    # Populates the slots -> nodes map.
    dict for {addr node} $nodes {
        foreach slotrange [dict get $node slots] {
            lassign [split $slotrange -] start end
            if {$end == {}} {set end $start}
            for {set j $start} {$j <= $end} {incr j} {
                dict set ::redis_cluster::slots($id) $j $addr
            }
        }
    }

    # Only retain unique entries in the startup nodes list
    set ::redis_cluster::startup_nodes($id) [lsort -unique $::redis_cluster::startup_nodes($id)]
}

# Free a redis_cluster handle.
proc ::redis_cluster::__method__close {id} {
    catch {
        set nodes $::redis_cluster::nodes($id)
        dict for {addr node} $nodes {
            catch {
                [dict get $node link] close
            }
        }
    }
    catch {unset ::redis_cluster::startup_nodes($id)}
    catch {unset ::redis_cluster::nodes($id)}
    catch {unset ::redis_cluster::slots($id)}
    catch {interp alias {} ::redis_cluster::instance$id {}}
}

proc ::redis_cluster::__dispatch__ {id method args} {
    if {[info command ::redis_cluster::__method__$method] eq {}} {
        # Get the keys from the command.
        set keys [::redis_cluster::get_keys_from_command $method $args]
        if {$keys eq {}} {
            error "Redis command '$method' is not supported by redis_cluster."
        }

        # Resolve the keys in the corresponding hash slot they hash to.
        set slot [::redis_cluster::get_slot_from_keys $keys]
        if {$slot eq {}} {
            error "Invalid command: multiple keys not hashing to the same slot."
        }

        # Get the node mapped to this slot.
        set node_addr [dict get $::redis_cluster::slots($id) $slot]
        if {$node_addr eq {}} {
            error "No mapped node for slot $slot."
        }

        # Execute the command in the node we think is the slot owner.
        set retry 100
        while {[incr retry -1]} {
            if {$retry < 5} {after 100}
            set node [dict get $::redis_cluster::nodes($id) $node_addr]
            set link [dict get $node link]
            if {[catch {$link $method {*}$args} e]} {
                if {$link eq {} || \
                    [string range $e 0 4] eq {MOVED} || \
                    [string range $e 0 2] eq {I/O} \
                } {
                    # MOVED redirection.
                    ::redis_cluster::__method__refresh_nodes_map $id
                    set node_addr [dict get $::redis_cluster::slots($id) $slot]
                    continue
                } elseif {[string range $e 0 2] eq {ASK}} {
                    # ASK redirection.
                    set node_addr [lindex $e 2]
                    continue
                } else {
                    # Non redirecting error.
                    error $e $::errorInfo $::errorCode
                }
            } else {
                # OK query went fine
                return $e
            }
        }
        error "Too many redirections or failures contacting Redis Cluster."
    } else {
        uplevel 1 [list ::redis_cluster::__method__$method $id] $args
    }
}

proc ::redis_cluster::get_keys_from_command {cmd argv} {
    set cmd [string tolower $cmd]
    # Most Redis commands get just one key as first argument.
    if {[lsearch -exact $::redis_cluster::plain_commands $cmd] != -1} {
        return [list [lindex $argv 0]]
    }

    # Special handling for other commands
    switch -exact $cmd {
        mget {return $argv}
        eval {return [lrange $argv 2 1+[lindex $argv 1]]}
        evalsha {return [lrange $argv 2 1+[lindex $argv 1]]}
    }

    # All the remaining commands are not handled.
    return {}
}

# Returns the CRC16 of the specified string.
# The CRC parameters are described in the Redis Cluster specification.
set ::redis_cluster::XMODEMCRC16Lookup {
    0x0000 0x1021 0x2042 0x3063 0x4084 0x50a5 0x60c6 0x70e7
    0x8108 0x9129 0xa14a 0xb16b 0xc18c 0xd1ad 0xe1ce 0xf1ef
    0x1231 0x0210 0x3273 0x2252 0x52b5 0x4294 0x72f7 0x62d6
    0x9339 0x8318 0xb37b 0xa35a 0xd3bd 0xc39c 0xf3ff 0xe3de
    0x2462 0x3443 0x0420 0x1401 0x64e6 0x74c7 0x44a4 0x5485
    0xa56a 0xb54b 0x8528 0x9509 0xe5ee 0xf5cf 0xc5ac 0xd58d
    0x3653 0x2672 0x1611 0x0630 0x76d7 0x66f6 0x5695 0x46b4
    0xb75b 0xa77a 0x9719 0x8738 0xf7df 0xe7fe 0xd79d 0xc7bc
    0x48c4 0x58e5 0x6886 0x78a7 0x0840 0x1861 0x2802 0x3823
    0xc9cc 0xd9ed 0xe98e 0xf9af 0x8948 0x9969 0xa90a 0xb92b
    0x5af5 0x4ad4 0x7ab7 0x6a96 0x1a71 0x0a50 0x3a33 0x2a12
    0xdbfd 0xcbdc 0xfbbf 0xeb9e 0x9b79 0x8b58 0xbb3b 0xab1a
    0x6ca6 0x7c87 0x4ce4 0x5cc5 0x2c22 0x3c03 0x0c60 0x1c41
    0xedae 0xfd8f 0xcdec 0xddcd 0xad2a 0xbd0b 0x8d68 0x9d49
    0x7e97 0x6eb6 0x5ed5 0x4ef4 0x3e13 0x2e32 0x1e51 0x0e70
    0xff9f 0xefbe 0xdfdd 0xcffc 0xbf1b 0xaf3a 0x9f59 0x8f78
    0x9188 0x81a9 0xb1ca 0xa1eb 0xd10c 0xc12d 0xf14e 0xe16f
    0x1080 0x00a1 0x30c2 0x20e3 0x5004 0x4025 0x7046 0x6067
    0x83b9 0x9398 0xa3fb 0xb3da 0xc33d 0xd31c 0xe37f 0xf35e
    0x02b1 0x1290 0x22f3 0x32d2 0x4235 0x5214 0x6277 0x7256
    0xb5ea 0xa5cb 0x95a8 0x8589 0xf56e 0xe54f 0xd52c 0xc50d
    0x34e2 0x24c3 0x14a0 0x0481 0x7466 0x6447 0x5424 0x4405
    0xa7db 0xb7fa 0x8799 0x97b8 0xe75f 0xf77e 0xc71d 0xd73c
    0x26d3 0x36f2 0x0691 0x16b0 0x6657 0x7676 0x4615 0x5634
    0xd94c 0xc96d 0xf90e 0xe92f 0x99c8 0x89e9 0xb98a 0xa9ab
    0x5844 0x4865 0x7806 0x6827 0x18c0 0x08e1 0x3882 0x28a3
    0xcb7d 0xdb5c 0xeb3f 0xfb1e 0x8bf9 0x9bd8 0xabbb 0xbb9a
    0x4a75 0x5a54 0x6a37 0x7a16 0x0af1 0x1ad0 0x2ab3 0x3a92
    0xfd2e 0xed0f 0xdd6c 0xcd4d 0xbdaa 0xad8b 0x9de8 0x8dc9
    0x7c26 0x6c07 0x5c64 0x4c45 0x3ca2 0x2c83 0x1ce0 0x0cc1
    0xef1f 0xff3e 0xcf5d 0xdf7c 0xaf9b 0xbfba 0x8fd9 0x9ff8
    0x6e17 0x7e36 0x4e55 0x5e74 0x2e93 0x3eb2 0x0ed1 0x1ef0
}

proc ::redis_cluster::crc16 {s} {
    set s [encoding convertto ascii $s]
    set crc 0
    foreach char [split $s {}] {
        scan $char %c byte
        set crc [expr {(($crc<<8)&0xffff) ^ [lindex $::redis_cluster::XMODEMCRC16Lookup [expr {(($crc>>8)^$byte) & 0xff}]]}]
    }
    return $crc
}

# Hash a single key returning the slot it belongs to, Implemented hash
# tags as described in the Redis Cluster specification.
proc ::redis_cluster::hash {key} {
    set keylen [string length $key]
    set s {}
    set e {}
    for {set s 0} {$s < $keylen} {incr s} {
        if {[string index $key $s] eq "\{"} break
    }

    if {[expr {$s == $keylen}]} {
        set res [expr {[crc16 $key] & 16383}]
        return $res
    }

    for {set e [expr {$s+1}]} {$e < $keylen} {incr e} {
        if {[string index $key $e] == "\}"} break
    }

    if {$e == $keylen || $e == [expr {$s+1}]} {
        set res [expr {[crc16 $key] & 16383}]
        return $res
    }

    set key_sub [string range $key [expr {$s+1}] [expr {$e-1}]]
    return [expr {[crc16 $key_sub] & 16383}]
}

# Return the slot the specified keys hash to.
# If the keys hash to multiple slots, an empty string is returned to
# signal that the command can't be run in Redis Cluster.
proc ::redis_cluster::get_slot_from_keys {keys} {
    set slot {}
    foreach k $keys {
        set s [::redis_cluster::hash $k]
        if {$slot eq {}} {
            set slot $s
        } elseif {$slot != $s} {
            return {} ; # Error
        }
    }
    return $slot
}
