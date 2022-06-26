## case from integration/replication-3 (redis 6.2.6)

start_server {tags {"repl"}} {
    start_server {} {
        test {First server should have role slave after SLAVEOF} {
            r -1 slaveof [srv 0 host] [srv 0 port]
            wait_for_condition 50 100 {
                [s -1 master_link_status] eq {up}
            } else {
                fail "Replication not started."
            }
        }

        if {$::accurate} {set numops 50000} else {set numops 5000}

        test {MASTER and SLAVE consistency with expire} {
            createComplexDataset r $numops useexpire
            after 4000 ;# Make sure everything expired before taking the digest
            r keys *   ;# Force DEL syntesizing to slave
            after 1000 ;# Wait another second. Now everything should be fine.
            wait_for_condition 100 50 {
                [r -1 dbsize] == [r dbsize]
            } else {
                fail "wait sync"
            }
            if {$::debug_evict_keys} {
                set slave_digest [r -1 debug digest-keys]
                set master_digest [r -1 debug digest-keys]
            } else {
                set slave_digest [r -1 debug digest]
                set master_digest [r -1 debug digest]
            }
            if {$master_digest ne $slave_digest} {
                set csv1 [csvdump r]
                set csv2 [csvdump {r -1}]
                set fd [open /tmp/repldump1.txt w]
                puts -nonewline $fd $csv1
                close $fd
                set fd [open /tmp/repldump2.txt w]
                puts -nonewline $fd $csv2
                close $fd
                puts "Master - Replica inconsistency"
                puts "Run diff -u against /tmp/repldump*.txt for more info"
            }
            assert_equal $master_digest $slave_digest
        }

        test {Slave is able to evict keys created in writable slaves} {
            # wait createComplexDataset 
            wait_for_condition 500 100 {
                [r dbsize] == [r -1 dbsize]
            } else {
                fail "Replicas and master offsets were unable to match *exactly*."
            }
            if {$::swap} {
                r -1 config set slave-read-only no
                r -1 FLUSHDB
            } else {
                r -1 select 5
                assert {[r -1 dbsize] == 0}
                r -1 config set slave-read-only no
            }
            r -1 set key1 1 ex 5
            r -1 set key2 2 ex 5
            r -1 set key3 3 ex 5
            if {$::debug_evict_keys} {
                wait_for_condition 100 20 {
                    [r -1 dbsize] == 3
                } else {
                    fail "wait evict fail"
                }
            }
            assert {[r -1 dbsize] == 3}
            after 6000
            r -1 dbsize
        } {0}
    }
}

