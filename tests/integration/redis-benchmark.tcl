source tests/support/benchmark.tcl


start_server {tags {"benchmark"}} {
    start_server {} {
        set master [srv -1 client]
        set master_host [srv -1 host]
        set master_port [srv -1 port]
        
        test {full test suite} {
            set cmd [redisbenchmark $master_host $master_port "-c 5 -n 1000 -e"]
            if {[catch { exec {*}$cmd } error]} {
                set first_line [lindex [split $error "\n"] 0]
                puts [colorstr red "redis-benchmark non zero code. first line: $first_line"]
                fail "redis-benchmark non zero code. first line: $first_line"
            }
        }

        test {multi-thread full test suite} {
            set cmd [redisbenchmark $master_host $master_port "--threads 10 -c 5 -n 1000 -e"]
            if {[catch { exec {*}$cmd } error]} {
                set first_line [lindex [split $error "\n"] 0]
                puts [colorstr red "redis-benchmark non zero code. first line: $first_line"]
                fail "redis-benchmark non zero code. first line: $first_line"
            }
        }

        test {pipelined full test suite} {
            set cmd [redisbenchmark $master_host $master_port "-P 9 -c 5 -n 10000 -e"]
            if {[catch { exec {*}$cmd } error]} {
                set first_line [lindex [split $error "\n"] 0]
                puts [colorstr red "redis-benchmark non zero code. first line: $first_line"]
                fail "redis-benchmark non zero code. first line: $first_line"
            }
        }

        test {arbitrary command} {
            set cmd [redisbenchmark $master_host $master_port "-c 5 -n 1000 -e RPUSH mylist element"]
            if {[catch { exec {*}$cmd } error]} {
                set first_line [lindex [split $error "\n"] 0]
                puts [colorstr red "redis-benchmark non zero code. first line: $first_line"]
                fail "redis-benchmark non zero code. first line: $first_line"
            }
        }
    }
}
