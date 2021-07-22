# Tcl client library - used by the Redis test
# Copyright (C) 2009-2014 Salvatore Sanfilippo
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

package require Tcl 8.5
package provide redis 0.1

namespace eval redis {}
set ::redis::id 0
array set ::redis::fd {}
array set ::redis::addr {}
array set ::redis::blocking {}
array set ::redis::deferred {}
array set ::redis::readraw {}
array set ::redis::reconnect {}
array set ::redis::tls {}
array set ::redis::callback {}
array set ::redis::state {} ;# State in non-blocking reply reading
array set ::redis::statestack {} ;# Stack of states, for nested mbulks

proc redis {{server 127.0.0.1} {port 6379} {defer 0} {tls 0} {tlsoptions {}} {readraw 0}} {
    if {$tls} {
        package require tls
        ::tls::init \
            -cafile "$::tlsdir/ca.crt" \
            -certfile "$::tlsdir/client.crt" \
            -keyfile "$::tlsdir/client.key" \
            {*}$tlsoptions
        set fd [::tls::socket $server $port]
    } else {
        set fd [socket $server $port]
    }
    fconfigure $fd -translation binary
    set id [incr ::redis::id]
    set ::redis::fd($id) $fd
    set ::redis::addr($id) [list $server $port]
    set ::redis::blocking($id) 1
    set ::redis::deferred($id) $defer
    set ::redis::readraw($id) $readraw
    set ::redis::reconnect($id) 0
    set ::redis::tls($id) $tls
    ::redis::redis_reset_state $id
    interp alias {} ::redis::redisHandle$id {} ::redis::__dispatch__ $id
}

# This is a wrapper to the actual dispatching procedure that handles
# reconnection if needed.
proc ::redis::__dispatch__ {id method args} {
    set errorcode [catch {::redis::__dispatch__raw__ $id $method $args} retval]
    if {$errorcode && $::redis::reconnect($id) && $::redis::fd($id) eq {}} {
        # Try again if the connection was lost.
        # FIXME: we don't re-select the previously selected DB, nor we check
        # if we are inside a transaction that needs to be re-issued from
        # scratch.
        set errorcode [catch {::redis::__dispatch__raw__ $id $method $args} retval]
    }
    return -code $errorcode $retval
}

proc ::redis::__dispatch__raw__ {id method argv} {
    set fd $::redis::fd($id)

    # Reconnect the link if needed.
    if {$fd eq {}} {
        lassign $::redis::addr($id) host port
        if {$::redis::tls($id)} {
            set ::redis::fd($id) [::tls::socket $host $port]
        } else {
            set ::redis::fd($id) [socket $host $port]
        }
        fconfigure $::redis::fd($id) -translation binary
        set fd $::redis::fd($id)
    }

    set blocking $::redis::blocking($id)
    set deferred $::redis::deferred($id)
    if {$blocking == 0} {
        if {[llength $argv] == 0} {
            error "Please provide a callback in non-blocking mode"
        }
        set callback [lindex $argv end]
        set argv [lrange $argv 0 end-1]
    }
    if {[info command ::redis::__method__$method] eq {}} {
        set cmd "*[expr {[llength $argv]+1}]\r\n"
        append cmd "$[string length $method]\r\n$method\r\n"
        foreach a $argv {
            append cmd "$[string length $a]\r\n$a\r\n"
        }
        ::redis::redis_write $fd $cmd
        if {[catch {flush $fd}]} {
            catch {close $fd}
            set ::redis::fd($id) {}
            return -code error "I/O error reading reply"
        }

        if {!$deferred} {
            if {$blocking} {
                ::redis::redis_read_reply $id $fd
            } else {
                # Every well formed reply read will pop an element from this
                # list and use it as a callback. So pipelining is supported
                # in non blocking mode.
                lappend ::redis::callback($id) $callback
                fileevent $fd readable [list ::redis::redis_readable $fd $id]
            }
        }
    } else {
        uplevel 1 [list ::redis::__method__$method $id $fd] $argv
    }
}

proc ::redis::__method__blocking {id fd val} {
    set ::redis::blocking($id) $val
    fconfigure $fd -blocking $val
}

proc ::redis::__method__reconnect {id fd val} {
    set ::redis::reconnect($id) $val
}

proc ::redis::__method__read {id fd} {
    ::redis::redis_read_reply $id $fd
}

proc ::redis::__method__write {id fd buf} {
    ::redis::redis_write $fd $buf
}

proc ::redis::__method__flush {id fd} {
    flush $fd
}

proc ::redis::__method__close {id fd} {
    catch {close $fd}
    catch {unset ::redis::fd($id)}
    catch {unset ::redis::addr($id)}
    catch {unset ::redis::blocking($id)}
    catch {unset ::redis::deferred($id)}
    catch {unset ::redis::readraw($id)}
    catch {unset ::redis::reconnect($id)}
    catch {unset ::redis::tls($id)}
    catch {unset ::redis::state($id)}
    catch {unset ::redis::statestack($id)}
    catch {unset ::redis::callback($id)}
    catch {interp alias {} ::redis::redisHandle$id {}}
}

proc ::redis::__method__channel {id fd} {
    return $fd
}

proc ::redis::__method__deferred {id fd val} {
    set ::redis::deferred($id) $val
}

proc ::redis::__method__readraw {id fd val} {
    set ::redis::readraw($id) $val
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

proc ::redis::redis_multi_bulk_read {id fd} {
    set count [redis_read_line $fd]
    if {$count == -1} return {}
    set l {}
    set err {}
    for {set i 0} {$i < $count} {incr i} {
        if {[catch {
            lappend l [redis_read_reply $id $fd]
        } e] && $err eq {}} {
            set err $e
        }
    }
    if {$err ne {}} {return -code error $err}
    return $l
}

proc ::redis::redis_read_map {id fd} {
    set count [redis_read_line $fd]
    if {$count == -1} return {}
    set d {}
    set err {}
    for {set i 0} {$i < $count} {incr i} {
        if {[catch {
            set k [redis_read_reply $id $fd] ; # key
            set v [redis_read_reply $id $fd] ; # value
            dict set d $k $v
        } e] && $err eq {}} {
            set err $e
        }
    }
    if {$err ne {}} {return -code error $err}
    return $d
}

proc ::redis::redis_read_line fd {
    string trim [gets $fd]
}

proc ::redis::redis_read_null fd {
    gets $fd
    return {}
}

proc ::redis::redis_read_bool fd {
    set v [redis_read_line $fd]
    if {$v == "t"} {return 1}
    if {$v == "f"} {return 0}
    return -code error "Bad protocol, '$v' as bool type"
}

proc ::redis::redis_read_reply {id fd} {
    if {$::redis::readraw($id)} {
        return [redis_read_line $fd]
    }

    while {1} {
        set type [read $fd 1]
        switch -exact -- $type {
            _ {return [redis_read_null $fd]}
            : -
            ( -
            + {return [redis_read_line $fd]}
            , {return [expr {double([redis_read_line $fd])}]}
            # {return [redis_read_bool $fd]}
            - {return -code error [redis_read_line $fd]}
            $ {return [redis_bulk_read $fd]}
            > -
            ~ -
            * {return [redis_multi_bulk_read $id $fd]}
            % {return [redis_read_map $id $fd]}
            | {
                # ignore attributes for now (nowhere to store them)
                redis_read_map $id $fd
                continue
            }
            default {
                if {$type eq {}} {
                    catch {close $fd}
                    set ::redis::fd($id) {}
                    return -code error "I/O error reading reply"
                }
                return -code error "Bad protocol, '$type' as reply type byte"
            }
        }
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
