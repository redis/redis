# A fake Redis node for replaying predefined/expected traffic with a client.
#
# Usage: tclsh fake_redis_node.tcl PORT COMMAND REPLY [ COMMAND REPLY [ ... ] ]
#
# Commands are given as space-separated strings, e.g. "GET foo", and replies as
# RESP-encoded replies minus the trailing \r\n, e.g. "+OK".

set port [lindex $argv 0];
set expected_traffic [lrange $argv 1 end];

# Reads and parses a command from a socket and returns it as a space-separated
# string, e.g. "set foo bar".
proc read_command {sock} {
    set char [read $sock 1]
    switch $char {
        * {
            set numargs [gets $sock]
            set result {}
            for {set i 0} {$i<$numargs} {incr i} {
                read $sock 1;       # dollar sign
                set len [gets $sock]
                set str [read $sock $len]
                gets $sock;         # trailing \r\n
                lappend result $str
            }
            return $result
        }
        {} {
            # EOF
            return {}
        }
        default {
            # Non-RESP command
            set rest [gets $sock]
            return "$char$rest"
        }
    }
}

proc accept {sock host port} {
    global expected_traffic
    foreach {expect_cmd reply} $expected_traffic {
        if {[eof $sock]} {break}
        set cmd [read_command $sock]
        if {[string equal -nocase $cmd $expect_cmd]} {
            puts $sock $reply
            flush $sock
        } else {
            puts $sock "-ERR unexpected command $cmd"
            break
        }
    }
    close $sock
}

set sockfd [socket -server accept -myaddr 127.0.0.1 $port]
after 5000 set done timeout
vwait done
close $sockfd

