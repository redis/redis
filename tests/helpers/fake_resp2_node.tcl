# A fake RESP2 Redis node for replaying predefined traffic with a client.
#
# Usage: tclsh fake_resp2_node.tcl PORT COMMAND REPLY [ COMMAND REPLY [ ... ] ]
#
# Like tests/helpers/fake_redis_node.tcl but with explicit binary-mode I/O so
# replies that contain internal CRLFs (multi-bulk arrays, bulk strings) are
# delivered byte-for-byte. Replies must include their full RESP framing,
# including the trailing CRLF.

set port [lindex $argv 0]
set expected_traffic [lrange $argv 1 end]

proc read_command {sock} {
    set ch [read $sock 1]
    if {$ch ne "*"} { return {} }
    set numargs [string trimright [gets $sock] "\r"]
    set cmd ""
    for {set i 0} {$i < $numargs} {incr i} {
        read $sock 1
        set len [string trimright [gets $sock] "\r"]
        lappend cmd [read $sock $len]
        gets $sock
    }
    return $cmd
}

proc accept {sock host port} {
    global expected_traffic
    fconfigure $sock -translation binary -buffering none
    foreach {expect_cmd reply} $expected_traffic {
        if {[eof $sock]} break
        set cmd [read_command $sock]
        if {$cmd eq {}} break
        if {[string equal -nocase $cmd $expect_cmd]} {
            puts -nonewline $sock $reply
            flush $sock
        } else {
            puts -nonewline $sock "-ERR unexpected command $cmd\r\n"
            flush $sock
            break
        }
    }
    close $sock
}

set sockfd [socket -server accept -myaddr 127.0.0.1 $port]
after 5000 set done timeout
vwait done
close $sockfd
