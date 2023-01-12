source tests/support/redis.tcl
source tests/support/util.tcl
source tests/support/response_transformers.tcl

set ::tlsdir "tests/tls"
set ::force_resp3 0

proc bg_complex_data {host port db ops tls} {
    set r [redis $host $port 0 $tls]
    $r client setname LOAD_HANDLER
    $r select $db
    createComplexDataset $r $ops
}

bg_complex_data [lindex $argv 0] [lindex $argv 1] [lindex $argv 2] [lindex $argv 3] [lindex $argv 4]
