proc start_bg_complex_data {host port db ops} {
    exec tclsh tests/helpers/bg_complex_data.tcl $host $port $db $ops &
}

proc stop_bg_complex_data {handle} {
    catch {exec /bin/kill -9 $handle}
}

start_server {tags {"repl"}} {
    start_server {} {

        set master [srv 0 client]
        set master_host [srv 0 host]
        set master_port [srv 0 port]
        set load_handle0 [start_bg_complex_data $master_host $master_port 9 100000]
        set load_handle1 [start_bg_complex_data $master_host $master_port 11 100000]
        set load_handle2 [start_bg_complex_data $master_host $master_port 12 100000]

        test {First server should have role slave after SLAVEOF} {
            r -1 slaveof [srv 0 host] [srv 0 port]
            after 1000
            s -1 role
        } {slave}

        test {Test replication with parallel clients writing in differnet DBs} {
            lappend slave [srv 0 client]
            after 5000
            stop_bg_complex_data $load_handle0
            stop_bg_complex_data $load_handle1
            stop_bg_complex_data $load_handle2
            set retry 10
            while {$retry && ([$master debug digest] ne [$slave debug digest])}\
            {
                after 1000
                incr retry -1
            }
            assert {[$master dbsize] > 0}

            if {[r debug digest] ne [r -1 debug digest]} {
                set csv1 [csvdump r]
                set csv2 [csvdump {r -1}]
                set fd [open /tmp/repldump1.txt w]
                puts -nonewline $fd $csv1
                close $fd
                set fd [open /tmp/repldump2.txt w]
                puts -nonewline $fd $csv2
                close $fd
                puts "Master - Slave inconsistency"
                puts "Run diff -u against /tmp/repldump*.txt for more info"
            }
            assert_equal [r debug digest] [r -1 debug digest]
        }
    }
}
