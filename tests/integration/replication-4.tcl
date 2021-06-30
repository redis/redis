start_server {tags {"repl network external:skip"}} {
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

        test {Test replication with parallel clients writing in different DBs} {
            after 5000
            stop_bg_complex_data $load_handle0
            stop_bg_complex_data $load_handle1
            stop_bg_complex_data $load_handle2
            wait_for_condition 100 100 {
                [$master debug digest] == [$slave debug digest]
            } else {
                set csv1 [csvdump r]
                set csv2 [csvdump {r -1}]
                set fd [open /tmp/repldump1.txt w]
                puts -nonewline $fd $csv1
                close $fd
                set fd [open /tmp/repldump2.txt w]
                puts -nonewline $fd $csv2
                close $fd
                fail "Master - Replica inconsistency, Run diff -u against /tmp/repldump*.txt for more info"
            }
            assert {[$master dbsize] > 0}
        }
    }
}

start_server {tags {"repl external:skip"}} {
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
            exec kill -SIGSTOP [srv 0 pid]
            wait_for_condition 100 100 {
                [catch {$master set foo bar}] != 0
            } else {
                fail "Master didn't become readonly"
            }
            catch {$master set foo bar} err
            assert_match {NOREPLICAS*} $err
            exec kill -SIGCONT [srv 0 pid]
        }
    }
}

start_server {tags {"repl external:skip"}} {
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

        test {Replication of SPOP command -- alsoPropagate() API} {
            $master del myset
            set size [expr 1+[randomInt 100]]
            set content {}
            for {set j 0} {$j < $size} {incr j} {
                lappend content [randomValue]
            }
            $master sadd myset {*}$content

            set count [randomInt 100]
            set result [$master spop myset $count]

            wait_for_condition 50 100 {
                [$master debug digest] eq [$slave debug digest]
            } else {
                fail "SPOP replication inconsistency"
            }
        }
    }
}
