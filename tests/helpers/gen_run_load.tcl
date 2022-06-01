source tests/support/redis.tcl

set ::tlsdir "tests/tls"
proc randpath args {
    set path [expr {int(rand()*[llength $args])}]
    uplevel 1 [lindex $args $path]
}

proc format_command {args} {
    set cmd "*[llength $args]\r\n"
    foreach a $args {
        append cmd "$[string length $a]\r\n$a\r\n"
    }
    set _ $cmd
}
proc gen_run_load {host port seconds counter tls db code} {
    set start_time [clock seconds]
    set r [redis $host $port 1 $tls]
    $r client setname LOAD_HANDLER
    $r select $db
    if {$seconds > 0} {
        while 1 {
            set errcode [catch {uplevel 0 $code} result]
            if {$result ne {}} {puts "$result"}
            if {[clock seconds]-$start_time > $seconds} {
                exit 0
            }
        }
    } else {
        while {$counter != 0} {
            set errcode [catch {uplevel 0 $code} result]
            if {$result ne {}} {puts "$result"}
            set counter [expr $counter - 1]
        }
    }
    
}

gen_run_load [lindex $argv 0] [lindex $argv 1] [lindex $argv 2] [lindex $argv 3] [lindex $argv 4] [lindex $argv 5] [lindex $argv 6]
