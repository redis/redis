start_server {tags {"repl external:skip"}} {
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

            # Make sure everything expired before taking the digest
            # createComplexDataset uses max expire time of 2 seconds
            wait_for_condition 50 100 {
                0 == [scan [regexp -inline {expires\=([\d]*)} [r -1 info keyspace]] expires=%d]
            } else {
                fail "expire didn't end"
            }

            # make sure the replica got all the DELs
            wait_for_ofs_sync [srv 0 client] [srv -1 client]

            if {[r debug digest] ne [r -1 debug digest]} {
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
            assert_equal [r debug digest] [r -1 debug digest]
        }

        test {Master can replicate command longer than client-query-buffer-limit on replica} {
            # Configure the master to have a bigger query buffer limit
            r config set client-query-buffer-limit 2000000
            r -1 config set client-query-buffer-limit 1048576
            # Write a very large command onto the master
            r set key [string repeat "x" 1100000]
            wait_for_condition 300 100 {
                [r -1 get key] eq [string repeat "x" 1100000]
            } else {
                fail "Unable to replicate command longer than client-query-buffer-limit"
            }
        }

        test {Slave is able to evict keys created in writable slaves} {
            r -1 select 5
            assert {[r -1 dbsize] == 0}
            r -1 config set slave-read-only no
            r -1 set key1 1 ex 5
            r -1 set key2 2 ex 5
            r -1 set key3 3 ex 5
            assert {[r -1 dbsize] == 3}
            after 6000
            r -1 dbsize
        } {0}

        test {Writable replica doesn't return expired keys} {
            r select 5
            assert {[r dbsize] == 0}
            r debug set-active-expire 0
            r set key1 5 px 10
            r set key2 5 px 10
            r -1 select 5
            wait_for_condition 50 100 {
                [r -1 dbsize] == 2 && [r -1 exists key1 key2] == 0
            } else {
                fail "Keys didn't replicate or didn't expire."
            }
            r -1 config set slave-read-only no
            assert_equal 2 [r -1 dbsize]    ; # active expire is off
            assert_equal 1 [r -1 incr key1] ; # incr expires and re-creates key1
            assert_equal -1 [r -1 ttl key1] ; # incr created key1 without TTL
            assert_equal {} [r -1 get key2] ; # key2 expired but not deleted
            assert_equal 2 [r -1 dbsize]
            # cleanup
            r debug set-active-expire 1
            r -1 del key1 key2
            r -1 config set slave-read-only yes
            r del key1 key2
        }

        test {PFCOUNT updates cache on readonly replica} {
            r select 5
            assert {[r dbsize] == 0}
            r pfadd key a b c d e f g h i j k l m n o p q
            set strval [r get key]
            r -1 select 5
            wait_for_condition 50 100 {
                [r -1 dbsize] == 1
            } else {
                fail "Replication timeout."
            }
            assert {$strval == [r -1 get key]}
            assert_equal 17 [r -1 pfcount key]
            assert {$strval != [r -1 get key]}; # cache updated
            # cleanup
            r del key
        }

        test {PFCOUNT doesn't use expired key on readonly replica} {
            r select 5
            assert {[r dbsize] == 0}
            r debug set-active-expire 0
            r pfadd key a b c d e f g h i j k l m n o p q
            r pexpire key 10
            r -1 select 5
            wait_for_condition 50 100 {
                [r -1 dbsize] == 1 && [r -1 exists key] == 0
            } else {
                fail "Key didn't replicate or didn't expire."
            }
            assert_equal [r -1 pfcount key] 0 ; # expired key not used
            assert_equal [r -1 dbsize] 1      ; # but it's also not deleted
            # cleanup
            r debug set-active-expire 1
            r del key
        }
    }
}
