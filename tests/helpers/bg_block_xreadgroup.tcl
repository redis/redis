source tests/support/redis.tcl
source tests/support/util.tcl

set ::tlsdir "tests/tls"

proc bg_block_xreadgroup {host port db ops tls} {
    set r [redis $host $port 0 $tls]
    $r client setname LOAD_HANDLER
    $r select $db

    catch {$r XGROUP CREATE bg_block_streams bg_group $ MKSTREAM}

    for {set j 0} {$j < $ops} {incr j} {
        $r xreadgroup GROUP bg_group bg_user COUNT 10 BLOCK 1000 STREAMS bg_block_streams >
    }
}

bg_block_xreadgroup [lindex $argv 0] [lindex $argv 1] [lindex $argv 2] [lindex $argv 3] [lindex $argv 4]
