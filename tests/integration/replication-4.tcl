start_server {tags {"repl"}} {
    start_server {} {

        set primary [srv -1 client]
        set primary_host [srv -1 host]
        set primary_port [srv -1 port]
        set slave [srv 0 client]

        set load_handle0 [start_bg_complex_data $primary_host $primary_port 9 100000]
        set load_handle1 [start_bg_complex_data $primary_host $primary_port 11 100000]
        set load_handle2 [start_bg_complex_data $primary_host $primary_port 12 100000]

        test {First server should have role slave after SLAVEOF} {
            $slave slaveof $primary_host $primary_port
            after 1000
            s 0 role
        } {slave}

        test {Test replication with parallel clients writing in differnet DBs} {
            after 5000
            stop_bg_complex_data $load_handle0
            stop_bg_complex_data $load_handle1
            stop_bg_complex_data $load_handle2
            set retry 10
            while {$retry && ([$primary debug digest] ne [$slave debug digest])}\
            {
                after 1000
                incr retry -1
            }
            assert {[$primary dbsize] > 0}

            if {[$primary debug digest] ne [$slave debug digest]} {
                set csv1 [csvdump r]
                set csv2 [csvdump {r -1}]
                set fd [open /tmp/repldump1.txt w]
                puts -nonewline $fd $csv1
                close $fd
                set fd [open /tmp/repldump2.txt w]
                puts -nonewline $fd $csv2
                close $fd
                puts "Primary - Replica inconsistency"
                puts "Run diff -u against /tmp/repldump*.txt for more info"
            }
            assert_equal [r debug digest] [r -1 debug digest]
        }
    }
}

start_server {tags {"repl"}} {
    start_server {} {
        set primary [srv -1 client]
        set primary_host [srv -1 host]
        set primary_port [srv -1 port]
        set slave [srv 0 client]

        test {First server should have role slave after SLAVEOF} {
            $slave slaveof $primary_host $primary_port
            wait_for_condition 50 100 {
                [s 0 primary_link_status] eq {up}
            } else {
                fail "Replication not started."
            }
        }

        test {With min-slaves-to-write (1,3): primary should be writable} {
            $primary config set min-slaves-max-lag 3
            $primary config set min-slaves-to-write 1
            $primary set foo bar
        } {OK}

        test {With min-slaves-to-write (2,3): primary should not be writable} {
            $primary config set min-slaves-max-lag 3
            $primary config set min-slaves-to-write 2
            catch {$primary set foo bar} e
            set e
        } {NOREPLICAS*}

        test {With min-slaves-to-write: primary not writable with lagged slave} {
            $primary config set min-slaves-max-lag 2
            $primary config set min-slaves-to-write 1
            assert {[$primary set foo bar] eq {OK}}
            $slave deferred 1
            $slave debug sleep 6
            after 4000
            catch {$primary set foo bar} e
            set e
        } {NOREPLICAS*}
    }
}

start_server {tags {"repl"}} {
    start_server {} {
        set primary [srv -1 client]
        set primary_host [srv -1 host]
        set primary_port [srv -1 port]
        set slave [srv 0 client]

        test {First server should have role slave after SLAVEOF} {
            $slave slaveof $primary_host $primary_port
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
                $primary {*}$cmd
            }

            set retry 10
            while {$retry && ([$primary debug digest] ne [$slave debug digest])}\
            {
                after 1000
                incr retry -1
            }
            assert {[$primary dbsize] > 0}
        }

        test {Replication of SPOP command -- alsoPropagate() API} {
            $primary del myset
            set size [expr 1+[randomInt 100]]
            set content {}
            for {set j 0} {$j < $size} {incr j} {
                lappend content [randomValue]
            }
            $primary sadd myset {*}$content

            set count [randomInt 100]
            set result [$primary spop myset $count]

            wait_for_condition 50 100 {
                [$primary debug digest] eq [$slave debug digest]
            } else {
                fail "SPOP replication inconsistency"
            }
        }
    }
}
