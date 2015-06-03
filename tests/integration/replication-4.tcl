proc start_bg_complex_data {host port db ops} {
    set tclsh [info nameofexecutable]
    exec $tclsh tests/helpers/bg_complex_data.tcl $host $port $db $ops &
}

proc stop_bg_complex_data {handle} {
    kill_proc2 $handle
}

start_server {tags {"repl"}} {
    start_server {} {

        set master [srv -1 client]
        set master_host [srv -1 host]
        set master_port [srv -1 port]
        set slave [srv 0 client]

        set load_handle0 [start_bg_complex_data $master_host $master_port 9 100000]
        set load_handle1 [start_bg_complex_data $master_host $master_port 11 100000]
        set load_handle2 [start_bg_complex_data $master_host $master_port 12 100000]

        test {First server should have role slave after SLAVEOF} {
            $slave slaveof $master_host $master_port
            after 1000
            s 0 role
        } {slave}

        test {Test replication with parallel clients writing in differnet DBs} {
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

            if {[$master debug digest] ne [$slave debug digest]} {
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

start_server {tags {"repl"}} {
    start_server {} {
        set master [srv -1 client]
        set master_host [srv -1 host]
        set master_port [srv -1 port]
        set slave [srv 0 client]

        test {First server should have role slave after SLAVEOF} {
            $slave slaveof $master_host $master_port
            wait_for_condition 50 100 {
                [s 0 master_link_status] eq {up}
            } else {
                fail "Replication not started."
            }
        }

        test {With min-slaves-to-write (1,3): master should be writable} {
            $master config set min-slaves-max-lag 3
            $master config set min-slaves-to-write 1
            $master set foo bar
        } {OK}

        test {With min-slaves-to-write (2,3): master should not be writable} {
            $master config set min-slaves-max-lag 3
            $master config set min-slaves-to-write 2
            catch {$master set foo bar} e
            set e
        } {NOREPLICAS*}

        test {With min-slaves-to-write: master not writable with lagged slave} {
            $master config set min-slaves-max-lag 2
            $master config set min-slaves-to-write 1
            assert {[$master set foo bar] eq {OK}}
            $slave deferred 1
            $slave debug sleep 6
            after 4000
            catch {$master set foo bar} e
            set e
        } {NOREPLICAS*}
    }
}

start_server {tags {"repl"}} {
    start_server {} {
        set master [srv -1 client]
        set master_host [srv -1 host]
        set master_port [srv -1 port]
        set slave [srv 0 client]

        test {First server should have role slave after SLAVEOF} {
            $slave slaveof $master_host $master_port
            wait_for_condition 50 100 {
                [s 0 role] eq {slave}
            } else {
                fail "Replication not started."
            }
        }

        test {Replication: commands with many arguments (issue #1221)} {
            # We now issue large MSET commands, that may trigger a specific
            # class of bugs, see issue #1221.
            for {set j 0} {$j < 100} {incr j} {
                set cmd [list mset]
                for {set x 0} {$x < 1000} {incr x} {
                    lappend cmd [randomKey] [randomValue]
                }
                $master {*}$cmd
            }

            set retry 10
            while {$retry && ([$master debug digest] ne [$slave debug digest])}\
            {
                after 1000
                incr retry -1
            }
            assert {[$master dbsize] > 0}
        }
    }
}
