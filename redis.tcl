# Tcl clinet library - used by test-redis.tcl script for now
# Copyright (C) 2009 Salvatore Sanfilippo
# Released under the BSD license like Redis itself
#
# Example usage:
#
# set r [redis 127.0.0.1 6379]
# $r lpush mylist foo
# $r lpush mylist bar
# $r lrange mylist 0 -1
# $r close
#
# Non blocking usage example:
#
# proc handlePong {r type reply} {
#     puts "PONG $type '$reply'"
#     if {$reply ne "PONG"} {
#         $r ping [list handlePong]
#     }
# }
# 
# set r [redis]
# $r blocking 0
# $r get fo [list handlePong]
#
# vwait forever

package provide redis 0.1

namespace eval redis {}
set ::redis::id 0
array set ::redis::fd {}
array set ::redis::blocking {}
array set ::redis::callback {}
array set ::redis::state {} ;# State in non-blocking reply reading
array set ::redis::statestack {} ;# Stack of states, for nested mbulks
array set ::redis::bulkarg {}
array set ::redis::multibulkarg {}

# Flag commands requiring last argument as a bulk write operation
foreach redis_bulk_cmd {
    set setnx rpush lpush lset lrem sadd srem sismember echo getset smove zadd zrem zscore zincrby append zrank zrevrank hget hdel hexists
} {
    set ::redis::bulkarg($redis_bulk_cmd) {}
}

# Flag commands requiring last argument as a bulk write operation
foreach redis_multibulk_cmd {
    mset msetnx hset
} {
    set ::redis::multibulkarg($redis_multibulk_cmd) {}
}

unset redis_bulk_cmd
unset redis_multibulk_cmd

proc redis {{server 127.0.0.1} {port 6379}} {
    set fd [socket $server $port]
    fconfigure $fd -translation binary
    set id [incr ::redis::id]
    set ::redis::fd($id) $fd
    set ::redis::blocking($id) 1
    ::redis::redis_reset_state $id
    interp alias {} ::redis::redisHandle$id {} ::redis::__dispatch__ $id
}

proc ::redis::__dispatch__ {id method args} {
    set fd $::redis::fd($id)
    set blocking $::redis::blocking($id)
    if {$blocking == 0} {
        if {[llength $args] == 0} {
            error "Please provide a callback in non-blocking mode"
        }
        set callback [lindex $args end]
        set args [lrange $args 0 end-1]
    }
    if {[info command ::redis::__method__$method] eq {}} {
        if {[info exists ::redis::bulkarg($method)]} {
            set cmd "$method "
            append cmd [join [lrange $args 0 end-1]]
            append cmd " [string length [lindex $args end]]\r\n"
            append cmd [lindex $args end]
            ::redis::redis_writenl $fd $cmd
        } elseif {[info exists ::redis::multibulkarg($method)]} {
            set cmd "*[expr {[llength $args]+1}]\r\n"
            append cmd "$[string length $method]\r\n$method\r\n"
            foreach a $args {
                append cmd "$[string length $a]\r\n$a\r\n"
            }
            ::redis::redis_write $fd $cmd
            flush $fd
        } else {
            set cmd "$method "
            append cmd [join $args]
            ::redis::redis_writenl $fd $cmd
        }
        if {$blocking} {
            ::redis::redis_read_reply $fd
        } else {
            # Every well formed reply read will pop an element from this
            # list and use it as a callback. So pipelining is supported
            # in non blocking mode.
            lappend ::redis::callback($id) $callback
            fileevent $fd readable [list ::redis::redis_readable $fd $id]
        }
    } else {
        uplevel 1 [list ::redis::__method__$method $id $fd] $args
    }
}

proc ::redis::__method__blocking {id fd val} {
    set ::redis::blocking($id) $val
    fconfigure $fd -blocking $val
}

proc ::redis::__method__close {id fd} {
    catch {close $fd}
    catch {unset ::redis::fd($id)}
    catch {unset ::redis::blocking($id)}
    catch {unset ::redis::state($id)}
    catch {unset ::redis::statestack($id)}
    catch {unset ::redis::callback($id)}
    catch {interp alias {} ::redis::redisHandle$id {}}
}

proc ::redis::__method__channel {id fd} {
    return $fd
}

proc ::redis::redis_write {fd buf} {
    puts -nonewline $fd $buf
}

proc ::redis::redis_writenl {fd buf} {
    redis_write $fd $buf
    redis_write $fd "\r\n"
    flush $fd
}

proc ::redis::redis_readnl {fd len} {
    set buf [read $fd $len]
    read $fd 2 ; # discard CR LF
    return $buf
}

proc ::redis::redis_bulk_read {fd} {
    set count [redis_read_line $fd]
    if {$count == -1} return {}
    set buf [redis_readnl $fd $count]
    return $buf
}

proc ::redis::redis_multi_bulk_read fd {
    set count [redis_read_line $fd]
    if {$count == -1} return {}
    set l {}
    for {set i 0} {$i < $count} {incr i} {
        lappend l [redis_read_reply $fd]
    }
    return $l
}

proc ::redis::redis_read_line fd {
    string trim [gets $fd]
}

proc ::redis::redis_read_reply fd {
    set type [read $fd 1]
    switch -exact -- $type {
        : -
        + {redis_read_line $fd}
        - {return -code error [redis_read_line $fd]}
        $ {redis_bulk_read $fd}
        * {redis_multi_bulk_read $fd}
        default {return -code error "Bad protocol, $type as reply type byte"}
    }
}

proc ::redis::redis_reset_state id {
    set ::redis::state($id) [dict create buf {} mbulk -1 bulk -1 reply {}]
    set ::redis::statestack($id) {}
}

proc ::redis::redis_call_callback {id type reply} {
    set cb [lindex $::redis::callback($id) 0]
    set ::redis::callback($id) [lrange $::redis::callback($id) 1 end]
    uplevel #0 $cb [list ::redis::redisHandle$id $type $reply]
    ::redis::redis_reset_state $id
}

# Read a reply in non-blocking mode.
proc ::redis::redis_readable {fd id} {
    if {[eof $fd]} {
        redis_call_callback $id eof {}
        ::redis::__method__close $id $fd
        return
    }
    if {[dict get $::redis::state($id) bulk] == -1} {
        set line [gets $fd]
        if {$line eq {}} return ;# No complete line available, return
        switch -exact -- [string index $line 0] {
            : -
            + {redis_call_callback $id reply [string range $line 1 end-1]}
            - {redis_call_callback $id err [string range $line 1 end-1]}
            $ {
                dict set ::redis::state($id) bulk \
                    [expr [string range $line 1 end-1]+2]
                if {[dict get $::redis::state($id) bulk] == 1} {
                    # We got a $-1, hack the state to play well with this.
                    dict set ::redis::state($id) bulk 2
                    dict set ::redis::state($id) buf "\r\n"
                    ::redis::redis_readable $fd $id
                }
            }
            * {
                dict set ::redis::state($id) mbulk [string range $line 1 end-1]
                # Handle *-1
                if {[dict get $::redis::state($id) mbulk] == -1} {
                    redis_call_callback $id reply {}
                }
            }
            default {
                redis_call_callback $id err \
                    "Bad protocol, $type as reply type byte"
            }
        }
    } else {
        set totlen [dict get $::redis::state($id) bulk]
        set buflen [string length [dict get $::redis::state($id) buf]]
        set toread [expr {$totlen-$buflen}]
        set data [read $fd $toread]
        set nread [string length $data]
        dict append ::redis::state($id) buf $data
        # Check if we read a complete bulk reply
        if {[string length [dict get $::redis::state($id) buf]] ==
            [dict get $::redis::state($id) bulk]} {
            if {[dict get $::redis::state($id) mbulk] == -1} {
                redis_call_callback $id reply \
                    [string range [dict get $::redis::state($id) buf] 0 end-2]
            } else {
                dict with ::redis::state($id) {
                    lappend reply [string range $buf 0 end-2]
                    incr mbulk -1
                    set bulk -1
                }
                if {[dict get $::redis::state($id) mbulk] == 0} {
                    redis_call_callback $id reply \
                        [dict get $::redis::state($id) reply]
                }
            }
        }
    }
}
