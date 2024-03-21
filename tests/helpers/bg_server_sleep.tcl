source tests/support/redis.tcl
source tests/support/util.tcl

proc bg_server_sleep {host port sec} {
    set r [redis $host $port 0]
    $r client setname SLEEP_HANDLER
    $r debug sleep $sec
}

bg_server_sleep [lindex $argv 0] [lindex $argv 1] [lindex $argv 2]