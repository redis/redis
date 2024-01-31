start_server {tags {"maxmemory" "external:skip"}} {
    r config set maxmemory 11mb
    r config set maxmemory-policy allkeys-lru
    set server_pid [s process_id]

    proc init_test {client_eviction} {
        r flushdb

        set prev_maxmemory_clients [r config get maxmemory-clients]
        if $client_eviction {
            r config set maxmemory-clients 3mb
            r client no-evict on
        } else {
            r config set maxmemory-clients 0
        }

        r config resetstat
        # fill 5mb using 50 keys of 100kb
        for {set j 0} {$j < 50} {incr j} {
            r setrange $j 100000 x
        }
        assert_equal [r dbsize] 50
    }
    
    # Return true if the eviction occurred (client or key) based on argument
    proc check_eviction_test {client_eviction} {
        set evicted_keys [s evicted_keys]
        set evicted_clients [s evicted_clients]
        set dbsize [r dbsize]
        
        if $client_eviction {
            return [expr $evicted_clients > 0 && $evicted_keys == 0 && $dbsize == 50]
        } else {
            return [expr $evicted_clients == 0 && $evicted_keys > 0 && $dbsize < 50]
        }
    }

    # Assert the eviction test passed (and prints some debug info on verbose)
    proc verify_eviction_test {client_eviction} {
        set evicted_keys [s evicted_keys]
        set evicted_clients [s evicted_clients]
        set dbsize [r dbsize]
        
        if $::verbose {
            puts "evicted keys: $evicted_keys"
            puts "evicted clients: $evicted_clients"
            puts "dbsize: $dbsize"
        }

        assert [check_eviction_test $client_eviction]
    }

    foreach {client_eviction} {false true} {
        set clients {}
        test "eviction due to output buffers of many MGET clients, client eviction: $client_eviction" {
            init_test $client_eviction

            for {set j 0} {$j < 20} {incr j} {
                set rr [redis_deferring_client]
                lappend clients $rr
            }
            
            # Generate client output buffers via MGET until we can observe some effect on 
            # keys / client eviction, or we time out.
            set t [clock seconds]
            while {![check_eviction_test $client_eviction] && [expr [clock seconds] - $t] < 20} {
                foreach rr $clients {
                    if {[catch {
                        $rr mget 1
                        $rr flush
                    } err]} {
                        lremove clients $rr
                    }
                }
            }

            verify_eviction_test $client_eviction
        }
        foreach rr $clients {
            $rr close
        }

        set clients {}
        test "eviction due to input buffer of a dead client, client eviction: $client_eviction" {
            init_test $client_eviction
            
            for {set j 0} {$j < 30} {incr j} {
                set rr [redis_deferring_client]
                lappend clients $rr
            }

            foreach rr $clients {
                if {[catch {
                    $rr write "*250\r\n"
                    for {set j 0} {$j < 249} {incr j} {
                        $rr write "\$1000\r\n"
                        $rr write [string repeat x 1000]
                        $rr write "\r\n"
                        $rr flush
                    }
                }]} {
                    lremove clients $rr
                }
            }

            verify_eviction_test $client_eviction
        }
        foreach rr $clients {
            $rr close
        }

        set clients {}
        test "eviction due to output buffers of pubsub, client eviction: $client_eviction" {
            init_test $client_eviction

            for {set j 0} {$j < 20} {incr j} {
                set rr [redis_client]
                lappend clients $rr
            }

            foreach rr $clients {
                $rr subscribe bla
            }

            # Generate client output buffers via PUBLISH until we can observe some effect on 
            # keys / client eviction, or we time out.
            set bigstr [string repeat x 100000]
            set t [clock seconds]
            while {![check_eviction_test $client_eviction] && [expr [clock seconds] - $t] < 20} {
                if {[catch { r publish bla $bigstr } err]} {
                    if $::verbose {
                        puts "Error publishing: $err"
                    }
                }
            }

            verify_eviction_test $client_eviction
        }
        foreach rr $clients {
            $rr close
        }
    }

}

start_server {tags {"maxmemory external:skip"}} {
    test "Without maxmemory small integers are shared" {
        r config set maxmemory 0
        r set a 1
        assert_refcount_morethan a 1
    }

    test "With maxmemory and non-LRU policy integers are still shared" {
        r config set maxmemory 1073741824
        r config set maxmemory-policy allkeys-random
        r set a 1
        assert_refcount_morethan a 1
    }

    test "With maxmemory and LRU policy integers are not shared" {
        r config set maxmemory 1073741824
        r config set maxmemory-policy allkeys-lru
        r set a 1
        r config set maxmemory-policy volatile-lru
        r set b 1
        assert_refcount 1 a
        assert_refcount 1 b
        r config set maxmemory 0
    }

    foreach policy {
        allkeys-random allkeys-lru allkeys-lfu volatile-lru volatile-lfu volatile-random volatile-ttl
    } {
        test "maxmemory - is the memory limit honoured? (policy $policy)" {
            # make sure to start with a blank instance
            r flushall
            # Get the current memory limit and calculate a new limit.
            # We just add 100k to the current memory size so that it is
            # fast for us to reach that limit.
            set used [s used_memory]
            set limit [expr {$used+100*1024}]
            r config set maxmemory $limit
            r config set maxmemory-policy $policy
            # Now add keys until the limit is almost reached.
            set numkeys 0
            while 1 {
                r setex [randomKey] 10000 x
                incr numkeys
                if {[s used_memory]+4096 > $limit} {
                    assert {$numkeys > 10}
                    break
                }
            }
            # If we add the same number of keys already added again, we
            # should still be under the limit.
            for {set j 0} {$j < $numkeys} {incr j} {
                r setex [randomKey] 10000 x
            }
            assert {[s used_memory] < ($limit+4096)}
        }
    }

    foreach policy {
        allkeys-random allkeys-lru volatile-lru volatile-random volatile-ttl
    } {
        test "maxmemory - only allkeys-* should remove non-volatile keys ($policy)" {
            # make sure to start with a blank instance
            r flushall
            # Get the current memory limit and calculate a new limit.
            # We just add 100k to the current memory size so that it is
            # fast for us to reach that limit.
            set used [s used_memory]
            set limit [expr {$used+100*1024}]
            r config set maxmemory $limit
            r config set maxmemory-policy $policy
            # Now add keys until the limit is almost reached.
            set numkeys 0
            while 1 {
                r set [randomKey] x
                incr numkeys
                if {[s used_memory]+4096 > $limit} {
                    assert {$numkeys > 10}
                    break
                }
            }
            # If we add the same number of keys already added again and
            # the policy is allkeys-* we should still be under the limit.
            # Otherwise we should see an error reported by Redis.
            set err 0
            for {set j 0} {$j < $numkeys} {incr j} {
                if {[catch {r set [randomKey] x} e]} {
                    if {[string match {*used memory*} $e]} {
                        set err 1
                    }
                }
            }
            if {[string match allkeys-* $policy]} {
                assert {[s used_memory] < ($limit+4096)}
            } else {
                assert {$err == 1}
            }
        }
    }

    foreach policy {
        volatile-lru volatile-lfu volatile-random volatile-ttl
    } {
        test "maxmemory - policy $policy should only remove volatile keys." {
            # make sure to start with a blank instance
            r flushall
            # Get the current memory limit and calculate a new limit.
            # We just add 100k to the current memory size so that it is
            # fast for us to reach that limit.
            set used [s used_memory]
            set limit [expr {$used+100*1024}]
            r config set maxmemory $limit
            r config set maxmemory-policy $policy
            # Now add keys until the limit is almost reached.
            set numkeys 0
            while 1 {
                # Odd keys are volatile
                # Even keys are non volatile
                if {$numkeys % 2} {
                    r setex "key:$numkeys" 10000 x
                } else {
                    r set "key:$numkeys" x
                }
                if {[s used_memory]+4096 > $limit} {
                    assert {$numkeys > 10}
                    break
                }
                incr numkeys
            }
            # Now we add the same number of volatile keys already added.
            # We expect Redis to evict only volatile keys in order to make
            # space.
            set err 0
            for {set j 0} {$j < $numkeys} {incr j} {
                catch {r setex "foo:$j" 10000 x}
            }
            # We should still be under the limit.
            assert {[s used_memory] < ($limit+4096)}
            # However all our non volatile keys should be here.
            for {set j 0} {$j < $numkeys} {incr j 2} {
                assert {[r exists "key:$j"]}
            }
        }
    }
}

# Calculate query buffer memory of slave
proc slave_query_buffer {srv} {
    set clients [split [$srv client list] "\r\n"]
    set c [lsearch -inline $clients *flags=S*]
    if {[string length $c] > 0} {
        assert {[regexp {qbuf=([0-9]+)} $c - qbuf]}
        assert {[regexp {qbuf-free=([0-9]+)} $c - qbuf_free]}
        return [expr $qbuf + $qbuf_free]
    }
    return 0
}

proc test_slave_buffers {test_name cmd_count payload_len limit_memory pipeline} {
    start_server {tags {"maxmemory external:skip"}} {
        start_server {} {
        set slave_pid [s process_id]
        test "$test_name" {
            set slave [srv 0 client]
            set slave_host [srv 0 host]
            set slave_port [srv 0 port]
            set master [srv -1 client]
            set master_host [srv -1 host]
            set master_port [srv -1 port]

            # Disable slow log for master to avoid memory growth in slow env.
            $master config set slowlog-log-slower-than -1

            # add 100 keys of 100k (10MB total)
            for {set j 0} {$j < 100} {incr j} {
                $master setrange "key:$j" 100000 asdf
            }

            # make sure master doesn't disconnect slave because of timeout
            $master config set repl-timeout 1200 ;# 20 minutes (for valgrind and slow machines)
            $master config set maxmemory-policy allkeys-random
            $master config set client-output-buffer-limit "replica 100000000 100000000 300"
            $master config set repl-backlog-size [expr {10*1024}]

            # disable latency tracking
            $master config set latency-tracking no
            $slave config set latency-tracking no

            $slave slaveof $master_host $master_port
            wait_for_condition 50 100 {
                [s 0 master_link_status] eq {up}
            } else {
                fail "Replication not started."
            }

            # measure used memory after the slave connected and set maxmemory
            set orig_used [s -1 used_memory]
            set orig_client_buf [s -1 mem_clients_normal]
            set orig_mem_not_counted_for_evict [s -1 mem_not_counted_for_evict]
            set orig_used_no_repl [expr {$orig_used - $orig_mem_not_counted_for_evict}]
            set limit [expr {$orig_used - $orig_mem_not_counted_for_evict + 32*1024}]

            if {$limit_memory==1} {
                $master config set maxmemory $limit
            }

            # put the slave to sleep
            set rd_slave [redis_deferring_client]
            pause_process $slave_pid

            # send some 10mb worth of commands that don't increase the memory usage
            if {$pipeline == 1} {
                set rd_master [redis_deferring_client -1]
                for {set k 0} {$k < $cmd_count} {incr k} {
                    $rd_master setrange key:0 0 [string repeat A $payload_len]
                }
                for {set k 0} {$k < $cmd_count} {incr k} {
                    $rd_master read
                }
            } else {
                for {set k 0} {$k < $cmd_count} {incr k} {
                    $master setrange key:0 0 [string repeat A $payload_len]
                }
            }

            set new_used [s -1 used_memory]
            set slave_buf [s -1 mem_clients_slaves]
            set client_buf [s -1 mem_clients_normal]
            set mem_not_counted_for_evict [s -1 mem_not_counted_for_evict]
            set used_no_repl [expr {$new_used - $mem_not_counted_for_evict - [slave_query_buffer $master]}]
            # we need to exclude replies buffer and query buffer of replica from used memory.
            # removing the replica (output) buffers is done so that we are able to measure any other
            # changes to the used memory and see that they're insignificant (the test's purpose is to check that
            # the replica buffers are counted correctly, so the used memory growth after deducting them
            # should be nearly 0).
            # we remove the query buffers because on slow test platforms, they can accumulate many ACKs.
            set delta [expr {($used_no_repl - $client_buf) - ($orig_used_no_repl - $orig_client_buf)}]

            assert {[$master dbsize] == 100}
            assert {$slave_buf > 2*1024*1024} ;# some of the data may have been pushed to the OS buffers
            set delta_max [expr {$cmd_count / 2}] ;# 1 byte unaccounted for, with 1M commands will consume some 1MB
            assert {$delta < $delta_max && $delta > -$delta_max}

            $master client kill type slave
            set info_str [$master info memory]
            set killed_used [getInfoProperty $info_str used_memory]
            set killed_mem_not_counted_for_evict [getInfoProperty $info_str mem_not_counted_for_evict]
            set killed_slave_buf [s -1 mem_clients_slaves]
            # we need to exclude replies buffer and query buffer of slave from used memory after kill slave
            set killed_used_no_repl [expr {$killed_used - $killed_mem_not_counted_for_evict - [slave_query_buffer $master]}]
            set delta_no_repl [expr {$killed_used_no_repl - $used_no_repl}]
            assert {[$master dbsize] == 100}
            assert {$killed_slave_buf == 0}
            assert {$delta_no_repl > -$delta_max && $delta_no_repl < $delta_max}

        }
        # unfreeze slave process (after the 'test' succeeded or failed, but before we attempt to terminate the server
        resume_process $slave_pid
        }
    }
}

# test that slave buffer are counted correctly
# we wanna use many small commands, and we don't wanna wait long
# so we need to use a pipeline (redis_deferring_client)
# that may cause query buffer to fill and induce eviction, so we disable it
test_slave_buffers {slave buffer are counted correctly} 1000000 10 0 1

# test that slave buffer don't induce eviction
# test again with fewer (and bigger) commands without pipeline, but with eviction
test_slave_buffers "replica buffer don't induce eviction" 100000 100 1 0

start_server {tags {"maxmemory external:skip"}} {
    test {Don't rehash if used memory exceeds maxmemory after rehash} {
        r config set latency-tracking no
        r config set maxmemory 0
        r config set maxmemory-policy allkeys-random

        # Next rehash size is 8192, that will eat 64k memory
        populate 4095 "" 1

        set used [s used_memory]
        set limit [expr {$used + 10*1024}]
        r config set maxmemory $limit

        # Adding a key to meet the 1:1 radio.
        r set k0 v0
        # The dict has reached 4096, it can be resized in tryResizeHashTables in cron,
        # or we add a key to let it check whether it can be resized.
        r set k1 v1
        # Next writing command will trigger evicting some keys if last
        # command trigger DB dict rehash
        r set k2 v2
        # There must be 4098 keys because redis doesn't evict keys.
        r dbsize
    } {4098}
}

start_server {tags {"maxmemory external:skip"}} {
    test {client tracking don't cause eviction feedback loop} {
        r config set latency-tracking no
        r config set maxmemory 0
        r config set maxmemory-policy allkeys-lru
        r config set maxmemory-eviction-tenacity 100

        # 10 clients listening on tracking messages
        set clients {}
        for {set j 0} {$j < 10} {incr j} {
            lappend clients [redis_deferring_client]
        }
        foreach rd $clients {
            $rd HELLO 3
            $rd read ; # Consume the HELLO reply
            $rd CLIENT TRACKING on
            $rd read ; # Consume the CLIENT reply
        }

        # populate 300 keys, with long key name and short value
        for {set j 0} {$j < 300} {incr j} {
            set key $j[string repeat x 1000]
            r set $key x

            # for each key, enable caching for this key
            foreach rd $clients {
                $rd get $key
                $rd read
            }
        }

        # we need to wait one second for the client querybuf excess memory to be
        # trimmed by cron, otherwise the INFO used_memory and CONFIG maxmemory
        # below (on slow machines) won't be "atomic" and won't trigger eviction.
        after 1100

        # set the memory limit which will cause a few keys to be evicted
        # we need to make sure to evict keynames of a total size of more than
        # 16kb since the (PROTO_REPLY_CHUNK_BYTES), only after that the
        # invalidation messages have a chance to trigger further eviction.
        set used [s used_memory]
        set limit [expr {$used - 40000}]
        r config set maxmemory $limit

        # make sure some eviction happened
        set evicted [s evicted_keys]
        if {$::verbose} { puts "evicted: $evicted" }

        # make sure we didn't drain the database
        assert_range [r dbsize] 200 300

        assert_range $evicted 10 50
        foreach rd $clients {
            $rd read ;# make sure we have some invalidation message waiting
            $rd close
        }

        # eviction continues (known problem described in #8069)
        # for now this test only make sures the eviction loop itself doesn't
        # have feedback loop
        set evicted [s evicted_keys]
        if {$::verbose} { puts "evicted: $evicted" }
    }
}

start_server {tags {"maxmemory" "external:skip"}} {
    test {propagation with eviction} {
        set repl [attach_to_replication_stream]

        r set asdf1 1
        r set asdf2 2
        r set asdf3 3

        r config set maxmemory-policy allkeys-lru
        r config set maxmemory 1

        wait_for_condition 5000 10 {
            [r dbsize] eq 0
        } else {
            fail "Not all keys have been evicted"
        }

        r config set maxmemory 0
        r config set maxmemory-policy noeviction

        r set asdf4 4

        assert_replication_stream $repl {
            {select *}
            {set asdf1 1}
            {set asdf2 2}
            {set asdf3 3}
            {del asdf*}
            {del asdf*}
            {del asdf*}
            {set asdf4 4}
        }
        close_replication_stream $repl

        r config set maxmemory 0
        r config set maxmemory-policy noeviction
    }
}

start_server {tags {"maxmemory" "external:skip"}} {
    test {propagation with eviction in MULTI} {
        set repl [attach_to_replication_stream]

        r config set maxmemory-policy allkeys-lru

        r multi
        r incr x
        r config set maxmemory 1
        r incr x
        assert_equal [r exec] {1 OK 2}

        wait_for_condition 5000 10 {
            [r dbsize] eq 0
        } else {
            fail "Not all keys have been evicted"
        }

        assert_replication_stream $repl {
            {multi}
            {select *}
            {incr x}
            {incr x}
            {exec}
            {del x}
        }
        close_replication_stream $repl

        r config set maxmemory 0
        r config set maxmemory-policy noeviction
    }
}

start_server {tags {"maxmemory" "external:skip"}} {
    test {lru/lfu value of the key just added} {
        r config set maxmemory-policy allkeys-lru
        r set foo a
        assert {[r object idletime foo] <= 2}
        r del foo
        r set foo 1
        r get foo
        assert {[r object idletime foo] <= 2}

        r config set maxmemory-policy allkeys-lfu
        r del foo 
        r set foo a
        assert {[r object freq foo] == 5}
    }
}
