# This simple script is used in order to estimate the average PFAIL->FAIL
# state switch after a failure.

set ::sleep_time 10     ; # How much to sleep to trigger PFAIL.
set ::fail_port 30016   ; # Node to put in sleep.
set ::other_port 30001  ; # Node to use to monitor the flag switch.

proc avg vector {
    set sum 0.0
    foreach x $vector {
        set sum [expr {$sum+$x}]
    }
    expr {$sum/[llength $vector]}
}

set samples {}
while 1 {
    exec redis-cli -p $::fail_port debug sleep $::sleep_time > /dev/null &

    # Wait for fail? to appear.
    while 1 {
        set output [exec redis-cli -p $::other_port cluster nodes]
        if {[string match {*fail\?*} $output]} break
        after 100
    }

    puts "FAIL?"
    set start [clock milliseconds]

    # Wait for fail? to disappear.
    while 1 {
        set output [exec redis-cli -p $::other_port cluster nodes]
        if {![string match {*fail\?*} $output]} break
        after 100
    }

    puts "FAIL"
    set now [clock milliseconds]
    set elapsed [expr {$now-$start}]
    puts $elapsed
    lappend samples $elapsed

    puts "AVG([llength $samples]): [avg $samples]"

    # Wait for the instance to be available again.
    exec redis-cli -p $::fail_port ping

    # Wait for the fail flag to be cleared.
    after 2000
}
