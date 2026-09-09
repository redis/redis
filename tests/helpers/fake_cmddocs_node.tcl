# A fake Redis node that answers COMMAND DOCS with a deliberately malformed
# reply, used to test that redis-cli does not overflow its help-entry buffer
# when talking to a malicious/compromised server.
#
# Usage: tclsh fake_cmddocs_node.tcl PORT MODE ?LOGFILE?
#
# Each MODE targets one specific way the reply can make cliCountCommands() and
# cliInitCommandHelpEntries() disagree, or make either of them walk a key/value
# reply in pairs without checking that the last pair is complete:
#
#   MODE = nested   -> "subcommands" nested one level deeper than Redis ever
#                      goes, which the buggy client under-counts and then
#                      overflows while filling in help entries.
#   MODE = oddlen   -> odd-length top-level reply.
#   MODE = oddspec  -> even top-level reply, but a command's spec map is
#                      odd-length and its dangling key is "subcommands", so
#                      both the counting and the filling pass read past it.
#   MODE = oddsubs  -> odd-length "subcommands" map, so the filling pass walks
#                      one subcommand more than the counting pass reserved.
#   MODE = oddargs  -> odd-length argument map below "arguments". The counting
#                      pass never descends there, so this reaches
#                      cliAddCommandDocArg() with the reply intact.
#
# If LOGFILE is given, every command served is appended to it, one per line.
# The test uses that to prove redis-cli really asked for COMMAND DOCS instead
# of failing to connect and silently falling back to its static command table.
#
# Any command other than COMMAND [DOCS ...] is answered with +OK so the client
# handshake (SELECT/HELLO/etc., if any) can complete.

set port [lindex $argv 0]
set mode [lindex $argv 1]
set logfile [lindex $argv 2]

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
            # that recurses while filling writes past the allocation. 64 is
            # comfortably more than the 2 entries that get reserved, so the
            # stomped bytes land well outside the allocation.
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
        oddspec {
            # The spec map of "foo" is odd-length, so its last key has no
            # value. The dangling key is deliberately "subcommands": that is
            # the only key whose value cliCountCommands() dereferences, so this
            # is what makes the counting pass itself read past the array. The
            # filling pass reads the same missing element.
            set cmd_spec [arr [list [bulk "summary"] [bulk "a summary"] \
                                    [bulk "subcommands"]]]
            return [arr [list [bulk "foo"] $cmd_spec]]
        }
        oddsubs {
            # Odd-length "subcommands" map: the trailing subcommand name has no
            # spec map. The counting pass charges elements/2 == 1 subcommand,
            # while a filling pass that iterates unguarded runs twice - reading
            # past the array, and writing one entry more than was allocated.
            set subs     [arr [list [bulk "foo|bar"] [arr {}] [bulk "foo|extra"]]]
            set cmd_spec [arr [list [bulk "subcommands"] $subs]]
            return [arr [list [bulk "foo"] $cmd_spec]]
        }
        oddargs {
            # Odd-length argument map, reached through "arguments". Counting
            # only ever descends into "subcommands", so it neither miscounts
            # nor crashes here and the malformed map is handed to
            # cliAddCommandDocArg(), whose last pair has no value.
            set argmap   [arr [list [bulk "name"] [bulk "myarg"] [bulk "type"]]]
            set arguments [arr [list $argmap]]
            set cmd_spec [arr [list [bulk "arguments"] $arguments]]
            return [arr [list [bulk "foo"] $cmd_spec]]
        }
        default {
            error "unknown mode: $mode"
        }
    }
}

set reply [build_reply $mode]

# Append one served command to the log, if logging was requested. Opened and
# closed per line so the test always sees a flushed file, no matter how the
# script is torn down.
proc log_served {cmd} {
    global logfile
    if {$logfile eq ""} return
    set fd [open $logfile "a"]
    puts $fd $cmd
    close $fd
}

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
        log_served $cmd
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
