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

start_server {tags {"repl"}} {
    start_server {} {
        r -1 config set aof-use-rdb-preamble no
        test {First server should have role slave after SLAVEOF} {
            r -1 slaveof [srv 0 host] [srv 0 port]
            wait_for_condition 50 100 {
                [s -1 master_link_status] eq {up}
            } else {
                fail "Replication not started."
            }
        }

        set numops 20000 ;# Enough to trigger the Script Cache LRU eviction.
        r config set aof-use-rdb-preamble no
        # While we are at it, enable AOF to test it will be consistent as well
        # after the test.
        r config set appendonly yes

        test {MASTER and SLAVE consistency with EVALSHA replication} {
            array set oldsha {}
            for {set j 0} {$j < $numops} {incr j} {
                set key "key:$j"
                # Make sure to create scripts that have different SHA1s
                set script "return redis.call('incr','$key')"
                set sha1 [r eval "return redis.sha1hex(\"$script\")" 0]
                set oldsha($j) $sha1
                r eval $script 1 $key
                set res [r evalsha $sha1 1 $key]
                assert {$res == 2}
                # Additionally call one of the old scripts as well, at random.
                set rand_idx [randomInt $j]
                set rand_key "key:$rand_idx"
                set res [r evalsha $oldsha($rand_idx) 1 $rand_key]
                assert {$res > 2}

                # Trigger an AOF rewrite while we are half-way, this also
                # forces the flush of the script cache, and we will cover
                # more code as a result.
                if {$j == $numops / 2} {
                    catch {r bgrewriteaof}
                }
            }

            if {$::debug_evict_keys} {
                ## evict mode not support rewriteaof
                # wait_for_condition 500 100 {
                #     [r dbsize] == $numops &&
                #     [r -1 dbsize] == $numops &&
                #     [r debug digest-keys] eq [r -1 debug digest-keys]
                # } else {
                #     set csv1 [csvdump r]
                #     set csv2 [csvdump {r -1}]
                #     set fd [open /tmp/repldump1.txt w]
                #     puts -nonewline $fd $csv1
                #     close $fd
                #     set fd [open /tmp/repldump2.txt w]
                #     puts -nonewline $fd $csv2
                #     close $fd
                #     puts "Master - Replica inconsistency"
                #     puts "Run diff -u against /tmp/repldump*.txt for more info"
                # }
                # set old_digest [r debug digest-keys]
                # r config set appendonly no
                # r debug loadaof
                # set new_digest [r debug digest-keys]
                # assert {$old_digest eq $new_digest}
            } else {
                wait_for_condition 50 100 {
                    [r dbsize] == $numops &&
                    [r -1 dbsize] == $numops &&
                    [r debug digest] eq [r -1 debug digest]
                } else {
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
                set old_digest [r debug digest]
                r config set appendonly no
                r debug loadaof
                set new_digest [r debug digest]
                assert {$old_digest eq $new_digest}
            }      
        }

        test {SLAVE can reload "lua" AUX RDB fields of duplicated scripts} {
            # Force a Slave full resynchronization
            r debug change-repl-id
            r -1 client kill type master

            # Check that after a full resync the slave can still load
            # correctly the RDB file: such file will contain "lua" AUX
            # sections with scripts already in the memory of the master.

            wait_for_condition 500 100 {
                [s -1 master_link_status] eq {up}
            } else {
                fail "Replication not started."
            }

            if {$::debug_evict_keys} {
                wait_for_condition 500 100 {
                    [r debug digest-keys] eq [r -1 debug digest-keys]
                } else {
                    fail "DEBUG DIGEST-KEYS mismatch after full SYNC with many scripts"
                }
            } else {
                r debug swapout 
                r -1 debug swapout
                wait_for_condition 100 100 {
                    [r info keyspace] == [r info keyspace]
                } else {    
                    fail "Master - Replica sync fail"
                }
                wait_for_condition 50 100 {
                    [r debug digest-keys] eq [r -1 debug digest-keys]
                } else {
                    fail "DEBUG DIGEST mismatch after full SYNC with many scripts"
                }
            }
        }
    }
}
