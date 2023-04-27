source tests/support/redis.tcl

set ::tlsdir "tests/tls"

proc gen_write_load {host port seconds tls key} {
    set start_time [clock seconds]
    set r [redis $host $port 1 $tls]
    $r client setname LOAD_HANDLER
    $r select 9
    while 1 {
        if {$key == 0} {
            $r set [expr rand()] [expr rand()]
        } else {
            $r set $key [expr rand()]
        }
        if {[clock seconds]-$start_time > $seconds} {
            exit 0
        }
    }
}

gen_write_load [lindex $argv 0] [lindex $argv 1] [lindex $argv 2] [lindex $argv 3] [lindex $argv 4]
