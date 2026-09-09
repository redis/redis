# A fake Redis node that answers COMMAND DOCS with a deliberately malformed
# reply, used to test that redis-cli does not overflow its help-entry buffer
# when talking to a malicious/compromised server.
#
# Usage: tclsh fake_cmddocs_node.tcl PORT MODE
#
#   MODE = nested  -> the reply nests "subcommands" one level deeper than Redis
#                     ever does, which the buggy client under-counts and then
#                     overflows while filling in help entries.
#   MODE = oddlen  -> the top-level reply is an odd-length RESP2 array, which
#                     the buggy client both reads out of bounds and miscounts.
#
# Any command other than COMMAND [DOCS ...] is answered with +OK so the client
# handshake (SELECT/HELLO/etc., if any) can complete.

set port [lindex $argv 0]
set mode [lindex $argv 1]

# RESP2 encoders. Everything is emitted with explicit \r\n and the socket is
# put in binary output mode, so the bytes reach the client verbatim.
proc bulk {s} {
    return "\$[string length $s]\r\n$s\r\n"
}
proc arr {elements} {
    return "*[llength $elements]\r\n[join $elements ""]"
}

proc build_reply {mode} {
    switch -- $mode {
        nested {
            # Top-level command "foo" whose only spec key is "subcommands".
            # The single subcommand "foo|bar" in turn declares its own
            # "subcommands" map with many entries. cliCountCommands() only
            # reserves space for the first level of subcommands, so a client
            # that recurses while filling writes past the allocation.
            set nested_elts {}
            for {set i 0} {$i < 64} {incr i} {
                lappend nested_elts [bulk "foo|bar|baz$i"]
                lappend nested_elts [arr {}]
            }
            set nested_subs [arr $nested_elts]
            set sub_spec    [arr [list [bulk "subcommands"] $nested_subs]]
            set subs        [arr [list [bulk "foo|bar"] $sub_spec]]
            set cmd_spec    [arr [list [bulk "subcommands"] $subs]]
            return [arr [list [bulk "foo"] $cmd_spec]]
        }
        oddlen {
            # Three-element (odd) top-level array. A client iterating in pairs
            # reads one element past the array and counts one entry too few.
            return [arr [list [bulk "foo"] [arr {}] [bulk "extra"]]]
        }
        default {
            error "unknown mode: $mode"
        }
    }
}

set reply [build_reply $mode]

# Read one RESP command from the socket, returned as a space-separated string
# (e.g. "COMMAND DOCS"). Returns {} on EOF.
proc read_command {sock} {
    set char [read $sock 1]
    switch $char {
        * {
            set numargs [gets $sock]
            set result {}
            for {set i 0} {$i < $numargs} {incr i} {
                read $sock 1;       # dollar sign
                set len [gets $sock]
                set str [read $sock $len]
                gets $sock;         # trailing \r\n
                lappend result $str
            }
            return $result
        }
        {} {
            return {}
        }
        default {
            set rest [gets $sock]
            return "$char$rest"
        }
    }
}

proc accept {sock host port} {
    global reply
    # Input uses auto line-ending translation so gets() parses the RESP length
    # lines; output is binary so our crafted \r\n bytes are sent unchanged.
    fconfigure $sock -translation {auto binary} -buffering none
    while {![eof $sock]} {
        set cmd [read_command $sock]
        if {$cmd eq {}} break
        if {[string equal -nocase [lindex $cmd 0] "COMMAND"]} {
            puts -nonewline $sock $reply
            flush $sock
        } else {
            puts -nonewline $sock "+OK\r\n"
            flush $sock
        }
    }
    catch {close $sock}
}

set sockfd [socket -server accept -myaddr 127.0.0.1 $port]
after 10000 set done timeout
vwait done
close $sockfd
