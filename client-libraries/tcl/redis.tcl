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

package provide redis 0.1

namespace eval redis {}
set ::redis::id 0
array set ::redis::fd {}
array set ::redis::bulkarg {}
array set ::redis::multibulkarg {}

# Flag commands requiring last argument as a bulk write operation
foreach redis_bulk_cmd {
    set setnx rpush lpush lset lrem sadd srem sismember echo getset smove zadd zrem zscore
} {
    set ::redis::bulkarg($redis_bulk_cmd) {}
}

# Flag commands requiring last argument as a bulk write operation
foreach redis_multibulk_cmd {
    mset msetnx
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
    interp alias {} ::redis::redisHandle$id {} ::redis::__dispatch__ $id
}

proc ::redis::__dispatch__ {id method args} {
    set fd $::redis::fd($id)
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
        ::redis::redis_read_reply $fd
    } else {
        uplevel 1 [list ::redis::__method__$method $id $fd] $args
    }
}

proc ::redis::__method__close {id fd} {
    catch {close $fd}
    catch {unset ::redis::fd($id)}
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
