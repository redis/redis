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

        # Load some functions to be used later
        $master FUNCTION load replace {#!lua name=test
            redis.register_function{function_name='f_default_flags', callback=function(keys, args) return redis.call('get',keys[1]) end, flags={}}
            redis.register_function{function_name='f_no_writes', callback=function(keys, args) return redis.call('get',keys[1]) end, flags={'no-writes'}}
        }

        test {First server should have role slave after SLAVEOF} {
            $slave slaveof $master_host $master_port
            wait_replica_online $master
        }

        test {With min-slaves-to-write (1,3): master should be writable} {
            $master config set min-slaves-max-lag 3
            $master config set min-slaves-to-write 1
            assert_equal OK [$master set foo 123]
            assert_equal OK [$master eval "return redis.call('set','foo',12345)" 0]
        }

        test {With min-slaves-to-write (2,3): master should not be writable} {
            $master config set min-slaves-max-lag 3
            $master config set min-slaves-to-write 2
            assert_error "*NOREPLICAS*" {$master set foo bar}
            assert_error "*NOREPLICAS*" {$master eval "redis.call('set','foo','bar')" 0}
        }

        test {With min-slaves-to-write function without no-write flag} {
            assert_error "*NOREPLICAS*" {$master fcall f_default_flags 1 foo}
            assert_equal "12345" [$master fcall f_no_writes 1 foo]
        }

        test {With not enough good slaves, read in Lua script is still accepted} {
            $master config set min-slaves-max-lag 3
            $master config set min-slaves-to-write 1
            $master eval "redis.call('set','foo','bar')" 0

            $master config set min-slaves-to-write 2
            $master eval "return redis.call('get','foo')" 0
        } {bar}

        test {With min-slaves-to-write: master not writable with lagged slave} {
            $master config set min-slaves-max-lag 2
            $master config set min-slaves-to-write 1
            assert_equal OK [$master set foo 123]
            assert_equal OK [$master eval "return redis.call('set','foo',12345)" 0]
            # Killing a slave to make it become a lagged slave.
            exec kill -SIGSTOP [srv 0 pid]
            # Waiting for slave kill.
            wait_for_condition 100 100 {
                [catch {$master set foo 123}] != 0
            } else {
                fail "Master didn't become readonly"
            }
            assert_error "*NOREPLICAS*" {$master set foo 123}
            assert_error "*NOREPLICAS*" {$master eval "return redis.call('set','foo',12345)" 0}
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
                [s 0 master_link_status] eq {up}
            } else {
                fail "Replication not started."
            }
        }

        test {Replication of an expired key does not delete the expired key} {
            $master debug set-active-expire 0
            $master set k 1 ex 1
            wait_for_ofs_sync $master $slave
            exec kill -SIGSTOP [srv 0 pid]
            $master incr k
            after 1001
            # Stopping the replica for one second to makes sure the INCR arrives
            # to the replica after the key is logically expired.
            exec kill -SIGCONT [srv 0 pid]
            wait_for_ofs_sync $master $slave
            # Check that k is logically expired but is present in the replica.
            assert_equal 0 [$slave exists k]
            $slave debug object k ; # Raises exception if k is gone.
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

start_server {tags {"repl external:skip"}} {
    start_server {} {
        set master [srv -1 client]
        set master_host [srv -1 host]
        set master_port [srv -1 port]
        set replica [srv 0 client]

        test {First server should have role slave after SLAVEOF} {
            $replica slaveof $master_host $master_port
            wait_for_condition 50 100 {
                [s 0 role] eq {slave}
            } else {
                fail "Replication not started."
            }
            wait_for_sync $replica
        }

        test {Data divergence can happen under default conditions} {       
            $replica config set propagation-error-behavior ignore     
            $master debug replicate fake-command-1

            # Wait for replication to normalize
            $master set foo bar2
            $master wait 1 2000

            # Make sure we triggered the error, by finding the critical
            # message and the fake command.
            assert_equal [count_log_message 0 "fake-command-1"] 1
            assert_equal [count_log_message 0 "== CRITICAL =="] 1
        }

        test {Data divergence is allowed on writable replicas} {            
            $replica config set replica-read-only no
            $replica set number2 foo
            $master incrby number2 1
            $master wait 1 2000

            assert_equal [$master get number2] 1
            assert_equal [$replica get number2] foo

            assert_equal [count_log_message 0 "incrby"] 1
        }
    }
}
