source tests/support/redis.tcl
source tests/support/util.tcl

set ::tlsdir "tests/tls"

proc bg_xadd {host port db ops tls} {
    set r [redis $host $port 0 $tls]
    $r client setname LOAD_HANDLER
    $r select $db

    for {set j 0} {$j < $ops} {incr j} {
        randpath {
            $r xadd bg_block_streams MAXLEN 5000 * field1 value1 field2 value2
        } {
            $r xadd bg_block_streams MAXLEN 5000 * field3 value3 field4 value4 field5 value5
        }
    }
}

bg_xadd [lindex $argv 0] [lindex $argv 1] [lindex $argv 2] [lindex $argv 3] [lindex $argv 4]
