start_server {tags {"maxmemory external:skip"}} {
    test "Without maxmemory small integers are shared" {
        r config set maxmemory 0
        r set a 1
        assert {[r object refcount a] > 1}
    }

    test "With maxmemory and non-LRU policy integers are still shared" {
        r config set maxmemory 1073741824
        r config set maxmemory-policy allkeys-random
        r set a 1
        assert {[r object refcount a] > 1}
    }

    test "With maxmemory and LRU policy integers are not shared" {
        r config set maxmemory 1073741824
        r config set maxmemory-policy allkeys-lru
        r set a 1
        r config set maxmemory-policy volatile-lru
        r set b 1
        assert {[r object refcount a] == 1}
        assert {[r object refcount b] == 1}
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

            # add 100 keys of 100k (10MB total)
            for {set j 0} {$j < 100} {incr j} {
                $master setrange "key:$j" 100000 asdf
            }

            # make sure master doesn't disconnect slave because of timeout
            $master config set repl-timeout 1200 ;# 20 minutes (for valgrind and slow machines)
            $master config set maxmemory-policy allkeys-random
            $master config set client-output-buffer-limit "replica 100000000 100000000 300"
            $master config set repl-backlog-size [expr {10*1024}]

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
            exec kill -SIGSTOP $slave_pid

            # send some 10mb worth of commands that don't increase the memory usage
            if {$pipeline == 1} {
                set rd_master [redis_deferring_client -1]
                for {set k 0} {$k < $cmd_count} {incr k} {
                    $rd_master setrange key:0 0 [string repeat A $payload_len]
                }
                for {set k 0} {$k < $cmd_count} {incr k} {
                    #$rd_master read
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
            set used_no_repl [expr {$new_used - $mem_not_counted_for_evict}]
            set delta [expr {($used_no_repl - $client_buf) - ($orig_used_no_repl - $orig_client_buf)}]

            assert {[$master dbsize] == 100}
            assert {$slave_buf > 2*1024*1024} ;# some of the data may have been pushed to the OS buffers
            set delta_max [expr {$cmd_count / 2}] ;# 1 byte unaccounted for, with 1M commands will consume some 1MB
            assert {$delta < $delta_max && $delta > -$delta_max}

            $master client kill type slave
            set killed_used [s -1 used_memory]
            set killed_slave_buf [s -1 mem_clients_slaves]
            set killed_mem_not_counted_for_evict [s -1 mem_not_counted_for_evict]
            set killed_used_no_repl [expr {$killed_used - $killed_mem_not_counted_for_evict}]
            set delta_no_repl [expr {$killed_used_no_repl - $used_no_repl}]
            assert {$killed_slave_buf == 0}
            assert {$delta_no_repl > -$delta_max && $delta_no_repl < $delta_max}

        }
        # unfreeze slave process (after the 'test' succeeded or failed, but before we attempt to terminate the server
        exec kill -SIGCONT $slave_pid
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
        r config set maxmemory 0
        r config set maxmemory-policy allkeys-random

        # Next rehash size is 8192, that will eat 64k memory
        populate 4096 "" 1

        set used [s used_memory]
        set limit [expr {$used + 10*1024}]
        r config set maxmemory $limit
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
