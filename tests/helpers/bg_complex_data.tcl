source tests/support/redis.tcl
source tests/support/util.tcl

set ::tlsdir "tests/tls"

proc bg_complex_data {host port db ops tls} {
    set r [redis $host $port 0 $tls]
    $r client setname LOAD_HANDLER
    $r select $db
    createComplexDataset $r $ops
}

bg_complex_data [lindex $argv 0] [lindex $argv 1] [lindex $argv 2] [lindex $argv 3] [lindex $argv 4]
