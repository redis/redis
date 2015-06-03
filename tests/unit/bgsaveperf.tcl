start_server {tags {"other"}} {
    if {$::force_failure} {
        # This is used just for test suite development purposes.
        test {Failing test} {
            format err
        } {ok}
    }

    set iter1 1000000
    set iter2 100

    test {BGSAVE string copy on write latency} {
        waitForBgsave r
        r flushdb
        puts "Measuring BGSAVE for $iter1 strings"
        set start [clock clicks -milliseconds]
        for {set i 0} {$i < $iter1} {} {
            set args {}
            for {set j 0} {$j < $iter2} {incr j} {
                lappend args $i "abcdefghij"
                incr i
            }
            r mset {*}$args
        }
        set elapsed [expr [clock clicks -milliseconds]-$start]
        puts "time to create items                : [expr double($elapsed)/1000]"
        set start [clock clicks -milliseconds]
        r set 500 xyz
        set elapsed [expr [clock clicks -milliseconds]-$start]
        puts "time to modify first value (no save): [expr double($elapsed)/1000]"
        waitForBgsave r
        set bgstart [clock clicks -milliseconds]
        r bgsave
        waitForBgsave r
        set elapsed [expr [clock clicks -milliseconds]-$bgstart]
        puts "time for RO bgsave to complete      : [expr double($elapsed)/1000]"
        set start [clock clicks -milliseconds]
        set bgstart [clock clicks -milliseconds]
        r bgsave
        set elapsed [expr [clock clicks -milliseconds]-$start]
        puts "time to start bgsave                : [expr double($elapsed)/1000]"
        set start [clock clicks -milliseconds]
        r set 502 xyz
        set elapsed [expr [clock clicks -milliseconds]-$start]
        puts "time to modify first value (saving) : [expr double($elapsed)/1000]"
        set start [clock clicks -milliseconds]
        r set 503 xyz
        set elapsed [expr [clock clicks -milliseconds]-$start]
        puts "time to modify second value (saving): [expr double($elapsed)/1000]"
        waitForBgsave r
        set elapsed [expr [clock clicks -milliseconds]-$bgstart]
        puts "time for bgsave to complete         : [expr double($elapsed)/1000]"
        r flushdb
    } {OK}

    test {BGSAVE list copy on write latency} {
        waitForBgsave r
        r flushdb
        puts "Measuring BGSAVE for $iter1 strings in list"
        set start [clock clicks -milliseconds]
        for {set i 0} {$i < $iter1} {} {
            set args {}
            for {set j 0} {$j < $iter2} {incr j} {
                lappend args "abcdefghij"
                incr i
            }
            r rpush mylist {*}$args
        }
        set elapsed [expr [clock clicks -milliseconds]-$start]
        puts "time to create items                : [expr double($elapsed)/1000]"
        set start [clock clicks -milliseconds]
        r rpush mylist abcdefghij
        set elapsed [expr [clock clicks -milliseconds]-$start]
        puts "time to modify first value (no save): [expr double($elapsed)/1000]"
        waitForBgsave r
        set bgstart [clock clicks -milliseconds]
        r bgsave
        waitForBgsave r
        set elapsed [expr [clock clicks -milliseconds]-$bgstart]
        puts "time for RO bgsave to complete      : [expr double($elapsed)/1000]"
        set start [clock clicks -milliseconds]
        set bgstart [clock clicks -milliseconds]
        r bgsave
        set elapsed [expr [clock clicks -milliseconds]-$start]
        puts "time to start bgsave                : [expr double($elapsed)/1000]"
        set start [clock clicks -milliseconds]
        r rpush mylist abcdefghij
        set elapsed [expr [clock clicks -milliseconds]-$start]
        puts "time to modify first value (saving) : [expr double($elapsed)/1000]"
        set start [clock clicks -milliseconds]
        r rpush mylist abcdefghij
        set elapsed [expr [clock clicks -milliseconds]-$start]
        puts "time to modify second value (saving): [expr double($elapsed)/1000]"
        waitForBgsave r
        set elapsed [expr [clock clicks -milliseconds]-$bgstart]
        puts "time for bgsave to complete         : [expr double($elapsed)/1000]"
        r flushdb
    } {OK}

    test {BGSAVE hash dictionary copy on write latency} {
        waitForBgsave r
        r flushdb
        puts "Measuring BGSAVE for $iter1 strings in hash dictionary"
        set start [clock clicks -milliseconds]
        for {set i 0} {$i < $iter1} {} {
            set args {}
            for {set j 0} {$j < $iter2} {incr j} {
                lappend args $i "abcdefghij"
                incr i
            }
            r hmset myhash {*}$args
        }
        set elapsed [expr [clock clicks -milliseconds]-$start]
        puts "time to create items                : [expr double($elapsed)/1000]"
        set start [clock clicks -milliseconds]
        r hset myhash 501 xyz
        set elapsed [expr [clock clicks -milliseconds]-$start]
        puts "time to modify first value (no save): [expr double($elapsed)/1000]"
        waitForBgsave r
        set bgstart [clock clicks -milliseconds]
        r bgsave
        waitForBgsave r
        set elapsed [expr [clock clicks -milliseconds]-$bgstart]
        puts "time for RO bgsave to complete      : [expr double($elapsed)/1000]"
        set start [clock clicks -milliseconds]
        set bgstart [clock clicks -milliseconds]
        r bgsave
        set elapsed [expr [clock clicks -milliseconds]-$start]
        puts "time to start bgsave                : [expr double($elapsed)/1000]"
        set start [clock clicks -milliseconds]
        r hset myhash 502 xyz
        set elapsed [expr [clock clicks -milliseconds]-$start]
        puts "time to modify first value (saving) : [expr double($elapsed)/1000]"
        set start [clock clicks -milliseconds]
        r hset myhash 503 xyz
        set elapsed [expr [clock clicks -milliseconds]-$start]
        puts "time to modify second value (saving): [expr double($elapsed)/1000]"
        waitForBgsave r
        set elapsed [expr [clock clicks -milliseconds]-$bgstart]
        puts "time for bgsave to complete         : [expr double($elapsed)/1000]"
        r flushdb
    } {OK}

    test {BGSAVE large set copy on write latency} {
        waitForBgsave r
        r flushdb
        puts "Measuring BGSAVE for $iter1 strings in set"
        set start [clock clicks -milliseconds]
        for {set i 0} {$i < $iter1} {} {
            set args {}
            for {set j 0} {$j < $iter2} {incr j} {
                lappend args $i
                incr i
            }
            r sadd myset {*}$args
        }
        set elapsed [expr [clock clicks -milliseconds]-$start]
        puts "time to create items                : [expr double($elapsed)/1000]"
        set start [clock clicks -milliseconds]
        r sadd myset abc
        set elapsed [expr [clock clicks -milliseconds]-$start]
        puts "time to modify first value (no save): [expr double($elapsed)/1000]"
        waitForBgsave r
        set bgstart [clock clicks -milliseconds]
        r bgsave
        waitForBgsave r
        set elapsed [expr [clock clicks -milliseconds]-$bgstart]
        puts "time for RO bgsave to complete      : [expr double($elapsed)/1000]"
        set start [clock clicks -milliseconds]
        set bgstart [clock clicks -milliseconds]
        r bgsave
        set elapsed [expr [clock clicks -milliseconds]-$start]
        puts "time to start bgsave                : [expr double($elapsed)/1000]"
        set start [clock clicks -milliseconds]
        r sadd myset def
        set elapsed [expr [clock clicks -milliseconds]-$start]
        puts "time to modify first value (saving) : [expr double($elapsed)/1000]"
        set start [clock clicks -milliseconds]
        r sadd myset xyz
        set elapsed [expr [clock clicks -milliseconds]-$start]
        puts "time to modify second value (saving): [expr double($elapsed)/1000]"
        waitForBgsave r
        set elapsed [expr [clock clicks -milliseconds]-$bgstart]
        puts "time for bgsave to complete         : [expr double($elapsed)/1000]"
        r flushdb
    } {OK}

    test {BGSAVE large zset copy on write latency} {
        waitForBgsave r
        r flushdb
        puts "Measuring BGSAVE for $iter1 strings in ordered set"
        set start [clock clicks -milliseconds]
        for {set i 0} {$i < $iter1} {} {
            set args {}
            for {set j 0} {$j < $iter2} {incr j} {
                lappend args $i $i
                incr i
            }
            r zadd myzset {*}$args
        }
        set elapsed [expr [clock clicks -milliseconds]-$start]
        puts "time to create items                : [expr double($elapsed)/1000]"
        set start [clock clicks -milliseconds]
        r zadd myzset 501 9999
        set elapsed [expr [clock clicks -milliseconds]-$start]
        puts "time to modify first value (no save): [expr double($elapsed)/1000]"
        waitForBgsave r
        set bgstart [clock clicks -milliseconds]
        r bgsave
        waitForBgsave r
        set elapsed [expr [clock clicks -milliseconds]-$bgstart]
        puts "time for RO bgsave to complete      : [expr double($elapsed)/1000]"
        set start [clock clicks -milliseconds]
        set bgstart [clock clicks -milliseconds]
        r bgsave
        set elapsed [expr [clock clicks -milliseconds]-$start]
        puts "time to start bgsave                : [expr double($elapsed)/1000]"
        set start [clock clicks -milliseconds]
        r zadd myzset 502 9998
        set elapsed [expr [clock clicks -milliseconds]-$start]
        puts "time to modify first value (saving) : [expr double($elapsed)/1000]"
        set start [clock clicks -milliseconds]
        r zadd myzset 503 9997
        set elapsed [expr [clock clicks -milliseconds]-$start]
        puts "time to modify second value (saving): [expr double($elapsed)/1000]"
        waitForBgsave r
        set elapsed [expr [clock clicks -milliseconds]-$bgstart]
        puts "time for bgsave to complete         : [expr double($elapsed)/1000]"
        r flushdb
    } {OK}

}
