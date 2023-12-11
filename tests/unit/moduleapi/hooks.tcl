set testmodule [file normalize tests/modules/hooks.so]

tags "modules" {
    start_server [list overrides [list loadmodule "$testmodule" appendonly yes]] {
        test {Test module aof save on server start from empty} {
            assert {[r hooks.event_count persistence-syncaof-start] == 1}
        }

        test {Test clients connection / disconnection hooks} {
            for {set j 0} {$j < 2} {incr j} {
                set rd1 [redis_deferring_client]
                $rd1 close
            }
            assert {[r hooks.event_count client-connected] > 1}
            assert {[r hooks.event_count client-disconnected] > 1}
        }

        test {Test module client change event for blocked client} {
            set rd [redis_deferring_client]
            # select db other than 0
            $rd select 1
            # block on key
            $rd brpop foo 0
            # kill blocked client
            r client kill skipme yes
            # assert server is still up
            assert_equal [r ping] PONG
            $rd close
        } 

        test {Test module cron hook} {
            after 100
            assert {[r hooks.event_count cron-loop] > 0}
            set hz [r hooks.event_last cron-loop]
            assert_equal $hz 10
        }

        test {Test module loaded / unloaded hooks} {
            set othermodule [file normalize tests/modules/infotest.so]
            r module load $othermodule
            r module unload infotest
            assert_equal [r hooks.event_last module-loaded] "infotest"
            assert_equal [r hooks.event_last module-unloaded] "infotest"
        }

        test {Test module aofrw hook} {
            r debug populate 1000 foo 10000 ;# 10mb worth of data
            r config set rdbcompression no ;# rdb progress is only checked once in 2mb
            r BGREWRITEAOF
            waitForBgrewriteaof r
            assert_equal [string match {*module-event-persistence-aof-start*} [exec tail -20 < [srv 0 stdout]]] 1
            assert_equal [string match {*module-event-persistence-end*} [exec tail -20 < [srv 0 stdout]]] 1
        }

        test {Test module aof load and rdb/aof progress hooks} {
            # create some aof tail (progress is checked only once in 1000 commands)
            for {set j 0} {$j < 4000} {incr j} {
                r set "bar$j" x
            }
            # set some configs that will cause many loading progress events during aof loading
            r config set key-load-delay 500
            r config set dynamic-hz no
            r config set hz 500
            r DEBUG LOADAOF
            assert_equal [r hooks.event_last loading-aof-start] 0
            assert_equal [r hooks.event_last loading-end] 0
            assert {[r hooks.event_count loading-rdb-start] == 0}
            assert_lessthan 2 [r hooks.event_count loading-progress-rdb] ;# comes from the preamble section
            assert_lessthan 2 [r hooks.event_count loading-progress-aof]
            if {$::verbose} {
                puts "rdb progress events [r hooks.event_count loading-progress-rdb]"
                puts "aof progress events [r hooks.event_count loading-progress-aof]"
            }
        }
        # undo configs before next test
        r config set dynamic-hz yes
        r config set key-load-delay 0

        test {Test module rdb save hook} {
            # debug reload does: save, flush, load:
            assert {[r hooks.event_count persistence-syncrdb-start] == 0}
            assert {[r hooks.event_count loading-rdb-start] == 0}
            r debug reload
            assert {[r hooks.event_count persistence-syncrdb-start] == 1}
            assert {[r hooks.event_count loading-rdb-start] == 1}
        }

        test {Test key unlink hook} {
            r set testkey1 hello
            r del testkey1
            assert {[r hooks.event_count key-info-testkey1] == 1}
            assert_equal [r hooks.event_last key-info-testkey1] testkey1
            r lpush testkey1 hello
            r lpop testkey1
            assert {[r hooks.event_count key-info-testkey1] == 2}
            assert_equal [r hooks.event_last key-info-testkey1] testkey1
            r set testkey2 world
            r unlink testkey2
            assert {[r hooks.event_count key-info-testkey2] == 1}
            assert_equal [r hooks.event_last key-info-testkey2] testkey2
        }

        test {Test removed key event} {
            r set str abcd
            r set str abcde
            # For String Type value is returned
            assert_equal {abcd overwritten} [r hooks.is_key_removed str]
            assert_equal -1 [r hooks.pexpireat str]

            r del str
            assert_equal {abcde deleted} [r hooks.is_key_removed str]
            assert_equal -1 [r hooks.pexpireat str]

            # test int encoded string
            r set intstr 12345678
            # incr doesn't fire event
            r incr intstr
            catch {[r hooks.is_key_removed intstr]} output
            assert_match {ERR * removed} $output
            r del intstr
            assert_equal {12345679 deleted} [r hooks.is_key_removed intstr]

            catch {[r hooks.is_key_removed not-exists]} output
            assert_match {ERR * removed} $output

            r hset hash f v
            r hdel hash f
            assert_equal {0 deleted} [r hooks.is_key_removed hash]

            r hset hash f v a b
            r del hash
            assert_equal {2 deleted} [r hooks.is_key_removed hash]

            r lpush list 1
            r lpop list
            assert_equal {0 deleted} [r hooks.is_key_removed list]

            r lpush list 1 2 3
            r del list
            assert_equal {3 deleted} [r hooks.is_key_removed list]

            r sadd set 1
            r spop set
            assert_equal {0 deleted} [r hooks.is_key_removed set]

            r sadd set 1 2 3 4
            r del set
            assert_equal {4 deleted} [r hooks.is_key_removed set]

            r zadd zset 1 f
            r zpopmin zset
            assert_equal {0 deleted} [r hooks.is_key_removed zset]

            r zadd zset 1 f 2 d
            r del zset
            assert_equal {2 deleted} [r hooks.is_key_removed zset]

            r xadd stream 1-1 f v
            r xdel stream 1-1
            # Stream does not delete object when del entry
            catch {[r hooks.is_key_removed stream]} output
            assert_match {ERR * removed} $output
            r del stream
            assert_equal {0 deleted} [r hooks.is_key_removed stream]

            r xadd stream 2-1 f v
            r del stream
            assert_equal {1 deleted} [r hooks.is_key_removed stream]

            # delete key because of active expire
            set size [r dbsize]
            r set active-expire abcd px 1
            #ensure active expire
            wait_for_condition 50 100 {
                [r dbsize] == $size
            } else {
                fail "Active expire not trigger"
            }
            assert_equal {abcd expired} [r hooks.is_key_removed active-expire]
            # current time is greater than pexpireat
            set now [r time]
            set mill [expr ([lindex $now 0]*1000)+([lindex $now 1]/1000)]
            assert {$mill >= [r hooks.pexpireat active-expire]}

            # delete key because of lazy expire
            r debug set-active-expire 0
            r set lazy-expire abcd px 1
            after 10
            r get lazy-expire
            assert_equal {abcd expired} [r hooks.is_key_removed lazy-expire]
            set now [r time]
            set mill [expr ([lindex $now 0]*1000)+([lindex $now 1]/1000)]
            assert {$mill >= [r hooks.pexpireat lazy-expire]}
            r debug set-active-expire 1

            # delete key not yet expired
            set now [r time]
            set expireat [expr ([lindex $now 0]*1000)+([lindex $now 1]/1000)+1000000]
            r set not-expire abcd pxat $expireat
            r del not-expire
            assert_equal {abcd deleted} [r hooks.is_key_removed not-expire]
            assert_equal $expireat [r hooks.pexpireat not-expire]

            # Test key evict
            set used [expr {[s used_memory] - [s mem_not_counted_for_evict]}]
            set limit [expr {$used+100*1024}]
            set old_policy [lindex [r config get maxmemory-policy] 1]
            r config set maxmemory $limit
            # We set policy volatile-random, so only keys with ttl will be evicted
            r config set maxmemory-policy volatile-random
            r setex volatile-key 10000 x
            # We use SETBIT here, so we can set a big key and get the used_memory
            # bigger than maxmemory. Next command will evict volatile keys. We
            # can't use SET, as SET uses big input buffer, so it will fail.
            r setbit big-key 1600000 0 ;# this will consume 200kb
            r getbit big-key 0
            assert_equal {x evicted} [r hooks.is_key_removed volatile-key]
            r config set maxmemory-policy $old_policy
            r config set maxmemory 0
        } {OK} {needs:debug}

        test {Test flushdb hooks} {
            r flushdb
            assert_equal [r hooks.event_last flush-start] 9
            assert_equal [r hooks.event_last flush-end] 9
            r flushall
            assert_equal [r hooks.event_last flush-start] -1
            assert_equal [r hooks.event_last flush-end] -1
        }

        # replication related tests
        set master [srv 0 client]
        set master_host [srv 0 host]
        set master_port [srv 0 port]
        start_server {} {
            r module load $testmodule
            set replica [srv 0 client]
            set replica_host [srv 0 host]
            set replica_port [srv 0 port]
            $replica replicaof $master_host $master_port

            wait_replica_online $master

            test {Test master link up hook} {
                assert_equal [r hooks.event_count masterlink-up] 1
                assert_equal [r hooks.event_count masterlink-down] 0
            }

            test {Test role-replica hook} {
                assert_equal [r hooks.event_count role-replica] 1
                assert_equal [r hooks.event_count role-master] 0
                assert_equal [r hooks.event_last role-replica] [s 0 master_host]
            }

            test {Test replica-online hook} {
                assert_equal [r -1 hooks.event_count replica-online] 1
                assert_equal [r -1 hooks.event_count replica-offline] 0
            }

            test {Test master link down hook} {
                r client kill type master
                assert_equal [r hooks.event_count masterlink-down] 1

                wait_for_condition 50 100 {
                    [string match {*master_link_status:up*} [r info replication]]
                } else {
                    fail "Replica didn't reconnect"
                }

                assert_equal [r hooks.event_count masterlink-down] 1
                assert_equal [r hooks.event_count masterlink-up] 2
            }

            wait_for_condition 50 10 {
                [string match {*master_link_status:up*} [r info replication]]
            } else {
                fail "Can't turn the instance into a replica"
            }

            $replica replicaof no one

            test {Test role-master hook} {
                assert_equal [r hooks.event_count role-replica] 1
                assert_equal [r hooks.event_count role-master] 1
                assert_equal [r hooks.event_last role-master] {}
            }

            test {Test replica-offline hook} {
                assert_equal [r -1 hooks.event_count replica-online] 2
                assert_equal [r -1 hooks.event_count replica-offline] 2
            }
            # get the replica stdout, to be used by the next test
            set replica_stdout [srv 0 stdout]
        }

        test {Test swapdb hooks} {
            r swapdb 0 10
            assert_equal [r hooks.event_last swapdb-first] 0
            assert_equal [r hooks.event_last swapdb-second] 10
        }

        test {Test configchange hooks} {
            r config set rdbcompression no 
            assert_equal [r hooks.event_last config-change-count] 1
            assert_equal [r hooks.event_last config-change-first] rdbcompression
        }

        # look into the log file of the server that just exited
        test {Test shutdown hook} {
            assert_equal [string match {*module-event-shutdown*} [exec tail -5 < $replica_stdout]] 1
        }
    }

    start_server {} {
        test {OnLoad failure will handle un-registration} {
            catch {r module load $testmodule noload}
            r flushall
            r ping
        }
    }
}
