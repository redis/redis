proc test_memory_efficiency {range} {
    r flushall
    set rd [redis_deferring_client]
    set base_mem [s used_memory]
    set written 0
    for {set j 0} {$j < 10000} {incr j} {
        set key key:$j
        set val [string repeat A [expr {int(rand()*$range)}]]
        $rd set $key $val
        incr written [string length $key]
        incr written [string length $val]
        incr written 2 ;# A separator is the minimum to store key-value data.
    }
    for {set j 0} {$j < 10000} {incr j} {
        $rd read ; # Discard replies
    }

    set current_mem [s used_memory]
    set used [expr {$current_mem-$base_mem}]
    set efficiency [expr {double($written)/$used}]
    return $efficiency
}

start_server {tags {"memefficiency external:skip"}} {
    foreach {size_range expected_min_efficiency} {
        32    0.15
        64    0.25
        128   0.35
        1024  0.75
        16384 0.82
    } {
        test "Memory efficiency with values in range $size_range" {
            set efficiency [test_memory_efficiency $size_range]
            assert {$efficiency >= $expected_min_efficiency}
        }
    }
}

run_solo {defrag} {
    proc test_active_defrag {type} {
    if {[string match {*jemalloc*} [s mem_allocator]] && [r debug mallctl arenas.page] <= 8192} {
        test "Active defrag main dictionary: $type" {
            r config set hz 100
            r config set activedefrag no
            r config set active-defrag-threshold-lower 5
            r config set active-defrag-cycle-min 65
            r config set active-defrag-cycle-max 75
            r config set active-defrag-ignore-bytes 2mb
            r config set maxmemory 100mb
            r config set maxmemory-policy allkeys-lru

            populate 700000 asdf1 150
            populate 100 asdf1 150 0 false 1000
            populate 170000 asdf2 300
            populate 100 asdf2 300 0 false 1000

            assert {[scan [regexp -inline {expires\=([\d]*)} [r info keyspace]] expires=%d] > 0}
            after 120 ;# serverCron only updates the info once in 100ms
            set frag [s allocator_frag_ratio]
            if {$::verbose} {
                puts "frag $frag"
            }
            assert {$frag >= 1.4}

            r config set latency-monitor-threshold 5
            r latency reset
            r config set maxmemory 110mb ;# prevent further eviction (not to fail the digest test)
            set digest [debug_digest]
            catch {r config set activedefrag yes} e
            if {[r config get activedefrag] eq "activedefrag yes"} {
                # Wait for the active defrag to start working (decision once a
                # second).
                wait_for_condition 50 100 {
                    [s total_active_defrag_time] ne 0
                } else {
                    after 120 ;# serverCron only updates the info once in 100ms
                    puts [r info memory]
                    puts [r info stats]
                    puts [r memory malloc-stats]
                    fail "defrag not started."
                }

                # This test usually runs for a while, during this interval, we test the range.
                assert_range [s active_defrag_running] 65 75
                r config set active-defrag-cycle-min 1
                r config set active-defrag-cycle-max 1
                after 120 ;# serverCron only updates the info once in 100ms
                assert_range [s active_defrag_running] 1 1
                r config set active-defrag-cycle-min 65
                r config set active-defrag-cycle-max 75

                # Wait for the active defrag to stop working.
                wait_for_condition 2000 100 {
                    [s active_defrag_running] eq 0
                } else {
                    after 120 ;# serverCron only updates the info once in 100ms
                    puts [r info memory]
                    puts [r memory malloc-stats]
                    fail "defrag didn't stop."
                }

                # Test the fragmentation is lower.
                after 120 ;# serverCron only updates the info once in 100ms
                set frag [s allocator_frag_ratio]
                set max_latency 0
                foreach event [r latency latest] {
                    lassign $event eventname time latency max
                    if {$eventname == "active-defrag-cycle"} {
                        set max_latency $max
                    }
                }
                if {$::verbose} {
                    puts "frag $frag"
                    set misses [s active_defrag_misses]
                    set hits [s active_defrag_hits]
                    puts "hits: $hits"
                    puts "misses: $misses"
                    puts "max latency $max_latency"
                    puts [r latency latest]
                    puts [r latency history active-defrag-cycle]
                }
                assert {$frag < 1.1}
                # due to high fragmentation, 100hz, and active-defrag-cycle-max set to 75,
                # we expect max latency to be not much higher than 7.5ms but due to rare slowness threshold is set higher
                if {!$::no_latency} {
                    assert {$max_latency <= 30}
                }
            }
            # verify the data isn't corrupted or changed
            set newdigest [debug_digest]
            assert {$digest eq $newdigest}
            r save ;# saving an rdb iterates over all the data / pointers

            # if defrag is supported, test AOF loading too
            if {[r config get activedefrag] eq "activedefrag yes" && $type eq "standalone"} {
            test "Active defrag - AOF loading" {
                # reset stats and load the AOF file
                r config resetstat
                r config set key-load-delay -25 ;# sleep on average 1/25 usec
                r debug loadaof
                r config set activedefrag no
                # measure hits and misses right after aof loading
                set misses [s active_defrag_misses]
                set hits [s active_defrag_hits]

                after 120 ;# serverCron only updates the info once in 100ms
                set frag [s allocator_frag_ratio]
                set max_latency 0
                foreach event [r latency latest] {
                    lassign $event eventname time latency max
                    if {$eventname == "while-blocked-cron"} {
                        set max_latency $max
                    }
                }
                if {$::verbose} {
                    puts "AOF loading:"
                    puts "frag $frag"
                    puts "hits: $hits"
                    puts "misses: $misses"
                    puts "max latency $max_latency"
                    puts [r latency latest]
                    puts [r latency history "while-blocked-cron"]
                }
                # make sure we had defrag hits during AOF loading
                assert {$hits > 100000}
                # make sure the defragger did enough work to keep the fragmentation low during loading.
                # we cannot check that it went all the way down, since we don't wait for full defrag cycle to complete.
                assert {$frag < 1.4}
                # since the AOF contains simple (fast) SET commands (and the cron during loading runs every 1024 commands),
                # it'll still not block the loading for long periods of time.
                if {!$::no_latency} {
                    assert {$max_latency <= 40}
                }
            }
            } ;# Active defrag - AOF loading
        }
        r config set appendonly no
        r config set key-load-delay 0

        test "Active defrag eval scripts: $type" {
            r flushdb
            r script flush sync
            r config resetstat
            r config set hz 100
            r config set activedefrag no
            r config set active-defrag-threshold-lower 5
            r config set active-defrag-cycle-min 65
            r config set active-defrag-cycle-max 75
            r config set active-defrag-ignore-bytes 1500kb
            r config set maxmemory 0

            set n 50000

            # Populate memory with interleaving script-key pattern of same size
            set dummy_script "--[string repeat x 400]\nreturn "
            set rd [redis_deferring_client]
            for {set j 0} {$j < $n} {incr j} {
                set val "$dummy_script[format "%06d" $j]"
                $rd script load $val
                $rd set k$j $val
            }
            for {set j 0} {$j < $n} {incr j} {
                $rd read ; # Discard script load replies
                $rd read ; # Discard set replies
            }
            after 120 ;# serverCron only updates the info once in 100ms
            if {$::verbose} {
                puts "used [s allocator_allocated]"
                puts "rss [s allocator_active]"
                puts "frag [s allocator_frag_ratio]"
                puts "frag_bytes [s allocator_frag_bytes]"
            }
            assert_lessthan [s allocator_frag_ratio] 1.05

            # Delete all the keys to create fragmentation
            for {set j 0} {$j < $n} {incr j} { $rd del k$j }
            for {set j 0} {$j < $n} {incr j} { $rd read } ; # Discard del replies
            $rd close
            after 120 ;# serverCron only updates the info once in 100ms
            if {$::verbose} {
                puts "used [s allocator_allocated]"
                puts "rss [s allocator_active]"
                puts "frag [s allocator_frag_ratio]"
                puts "frag_bytes [s allocator_frag_bytes]"
            }
            assert_morethan [s allocator_frag_ratio] 1.4

            catch {r config set activedefrag yes} e
            if {[r config get activedefrag] eq "activedefrag yes"} {
            
                # wait for the active defrag to start working (decision once a second)
                wait_for_condition 50 100 {
                    [s total_active_defrag_time] ne 0
                } else {
                    after 120 ;# serverCron only updates the info once in 100ms
                    puts [r info memory]
                    puts [r info stats]
                    puts [r memory malloc-stats]
                    fail "defrag not started."
                }

                # wait for the active defrag to stop working
                wait_for_condition 500 100 {
                    [s active_defrag_running] eq 0
                } else {
                    after 120 ;# serverCron only updates the info once in 100ms
                    puts [r info memory]
                    puts [r memory malloc-stats]
                    fail "defrag didn't stop."
                }

                # test the fragmentation is lower
                after 120 ;# serverCron only updates the info once in 100ms
                if {$::verbose} {
                    puts "used [s allocator_allocated]"
                    puts "rss [s allocator_active]"
                    puts "frag [s allocator_frag_ratio]"
                    puts "frag_bytes [s allocator_frag_bytes]"
                }
                assert_lessthan_equal [s allocator_frag_ratio] 1.05
            }
            # Flush all script to make sure we don't crash after defragging them
            r script flush sync
        } {OK}

        test "Active defrag big keys: $type" {
            r flushdb
            r config resetstat
            r config set hz 100
            r config set activedefrag no
            r config set active-defrag-max-scan-fields 1000
            r config set active-defrag-threshold-lower 5
            r config set active-defrag-cycle-min 65
            r config set active-defrag-cycle-max 75
            r config set active-defrag-ignore-bytes 2mb
            r config set maxmemory 0
            r config set list-max-ziplist-size 5 ;# list of 10k items will have 2000 quicklist nodes
            r config set stream-node-max-entries 5
            r hmset hash h1 v1 h2 v2 h3 v3
            r lpush list a b c d
            r zadd zset 0 a 1 b 2 c 3 d
            r sadd set a b c d
            r xadd stream * item 1 value a
            r xadd stream * item 2 value b
            r xgroup create stream mygroup 0
            r xreadgroup GROUP mygroup Alice COUNT 1 STREAMS stream >

            # create big keys with 10k items
            set rd [redis_deferring_client]
            for {set j 0} {$j < 10000} {incr j} {
                $rd hset bighash $j [concat "asdfasdfasdf" $j]
                $rd lpush biglist [concat "asdfasdfasdf" $j]
                $rd zadd bigzset $j [concat "asdfasdfasdf" $j]
                $rd sadd bigset [concat "asdfasdfasdf" $j]
                $rd xadd bigstream * item 1 value a
            }
            for {set j 0} {$j < 50000} {incr j} {
                $rd read ; # Discard replies
            }

            # create some small items (effective in cluster-enabled)
            r set "{bighash}smallitem" val
            r set "{biglist}smallitem" val
            r set "{bigzset}smallitem" val
            r set "{bigset}smallitem" val
            r set "{bigstream}smallitem" val


            set expected_frag 1.7
            if {$::accurate} {
                # scale the hash to 1m fields in order to have a measurable the latency
                for {set j 10000} {$j < 1000000} {incr j} {
                    $rd hset bighash $j [concat "asdfasdfasdf" $j]
                }
                for {set j 10000} {$j < 1000000} {incr j} {
                    $rd read ; # Discard replies
                }
                # creating that big hash, increased used_memory, so the relative frag goes down
                set expected_frag 1.3
            }

            # add a mass of string keys
            for {set j 0} {$j < 500000} {incr j} {
                $rd setrange $j 150 a
            }
            for {set j 0} {$j < 500000} {incr j} {
                $rd read ; # Discard replies
            }
            assert_equal [r dbsize] 500015

            # create some fragmentation
            for {set j 0} {$j < 500000} {incr j 2} {
                $rd del $j
            }
            for {set j 0} {$j < 500000} {incr j 2} {
                $rd read ; # Discard replies
            }
            assert_equal [r dbsize] 250015

            # start defrag
            after 120 ;# serverCron only updates the info once in 100ms
            set frag [s allocator_frag_ratio]
            if {$::verbose} {
                puts "frag $frag"
            }
            assert {$frag >= $expected_frag}
            r config set latency-monitor-threshold 5
            r latency reset

            set digest [debug_digest]
            catch {r config set activedefrag yes} e
            if {[r config get activedefrag] eq "activedefrag yes"} {
                # wait for the active defrag to start working (decision once a second)
                wait_for_condition 50 100 {
                    [s total_active_defrag_time] ne 0
                } else {
                    after 120 ;# serverCron only updates the info once in 100ms
                    puts [r info memory]
                    puts [r info stats]
                    puts [r memory malloc-stats]
                    fail "defrag not started."
                }

                # wait for the active defrag to stop working
                wait_for_condition 500 100 {
                    [s active_defrag_running] eq 0
                } else {
                    after 120 ;# serverCron only updates the info once in 100ms
                    puts [r info memory]
                    puts [r memory malloc-stats]
                    fail "defrag didn't stop."
                }

                # test the fragmentation is lower
                after 120 ;# serverCron only updates the info once in 100ms
                set frag [s allocator_frag_ratio]
                set max_latency 0
                foreach event [r latency latest] {
                    lassign $event eventname time latency max
                    if {$eventname == "active-defrag-cycle"} {
                        set max_latency $max
                    }
                }
                if {$::verbose} {
                    puts "frag $frag"
                    set misses [s active_defrag_misses]
                    set hits [s active_defrag_hits]
                    puts "hits: $hits"
                    puts "misses: $misses"
                    puts "max latency $max_latency"
                    puts [r latency latest]
                    puts [r latency history active-defrag-cycle]
                }
                assert {$frag < 1.1}
                # due to high fragmentation, 100hz, and active-defrag-cycle-max set to 75,
                # we expect max latency to be not much higher than 7.5ms but due to rare slowness threshold is set higher
                if {!$::no_latency} {
                    assert {$max_latency <= 30}
                }
            }
            # verify the data isn't corrupted or changed
            set newdigest [debug_digest]
            assert {$digest eq $newdigest}
            r save ;# saving an rdb iterates over all the data / pointers
        } {OK}

        test "Active defrag pubsub: $type" {
            r flushdb
            r config resetstat
            r config set hz 100
            r config set activedefrag no
            r config set active-defrag-threshold-lower 5
            r config set active-defrag-cycle-min 65
            r config set active-defrag-cycle-max 75
            r config set active-defrag-ignore-bytes 1500kb
            r config set maxmemory 0

            # Populate memory with interleaving pubsub-key pattern of same size
            set n 50000
            set dummy_channel "[string repeat x 400]"
            set rd [redis_deferring_client]
            set rd_pubsub [redis_deferring_client]
            for {set j 0} {$j < $n} {incr j} {
                set channel_name "$dummy_channel[format "%06d" $j]"
                $rd_pubsub subscribe $channel_name
                $rd_pubsub read ; # Discard subscribe replies
                $rd_pubsub ssubscribe $channel_name
                $rd_pubsub read ; # Discard ssubscribe replies
                $rd set k$j $channel_name
                $rd read ; # Discard set replies
            }

            after 120 ;# serverCron only updates the info once in 100ms
            if {$::verbose} {
                puts "used [s allocator_allocated]"
                puts "rss [s allocator_active]"
                puts "frag [s allocator_frag_ratio]"
                puts "frag_bytes [s allocator_frag_bytes]"
            }
            assert_lessthan [s allocator_frag_ratio] 1.05

            # Delete all the keys to create fragmentation
            for {set j 0} {$j < $n} {incr j} { $rd del k$j }
            for {set j 0} {$j < $n} {incr j} { $rd read } ; # Discard del replies
            $rd close
            after 120 ;# serverCron only updates the info once in 100ms
            if {$::verbose} {
                puts "used [s allocator_allocated]"
                puts "rss [s allocator_active]"
                puts "frag [s allocator_frag_ratio]"
                puts "frag_bytes [s allocator_frag_bytes]"
            }
            assert_morethan [s allocator_frag_ratio] 1.35

            catch {r config set activedefrag yes} e
            if {[r config get activedefrag] eq "activedefrag yes"} {
            
                # wait for the active defrag to start working (decision once a second)
                wait_for_condition 50 100 {
                    [s total_active_defrag_time] ne 0
                } else {
                    after 120 ;# serverCron only updates the info once in 100ms
                    puts [r info memory]
                    puts [r info stats]
                    puts [r memory malloc-stats]
                    fail "defrag not started."
                }

                # wait for the active defrag to stop working
                wait_for_condition 500 100 {
                    [s active_defrag_running] eq 0
                } else {
                    after 120 ;# serverCron only updates the info once in 100ms
                    puts [r info memory]
                    puts [r memory malloc-stats]
                    fail "defrag didn't stop."
                }

                # test the fragmentation is lower
                after 120 ;# serverCron only updates the info once in 100ms
                if {$::verbose} {
                    puts "used [s allocator_allocated]"
                    puts "rss [s allocator_active]"
                    puts "frag [s allocator_frag_ratio]"
                    puts "frag_bytes [s allocator_frag_bytes]"
                }
                assert_lessthan_equal [s allocator_frag_ratio] 1.05
            }

            # Publishes some message to all the pubsub clients to make sure that
            # we didn't break the data structure.
            for {set j 0} {$j < $n} {incr j} {
                set channel "$dummy_channel[format "%06d" $j]"
                r publish $channel "hello"
                assert_equal "message $channel hello" [$rd_pubsub read] 
                $rd_pubsub unsubscribe $channel
                $rd_pubsub read
                r spublish $channel "hello"
                assert_equal "smessage $channel hello" [$rd_pubsub read] 
                $rd_pubsub sunsubscribe $channel
                $rd_pubsub read
            }
            $rd_pubsub close
        }

        if {$type eq "standalone"} { ;# skip in cluster mode
        test "Active defrag big list: $type" {
            r flushdb
            r config resetstat
            r config set hz 100
            r config set activedefrag no
            r config set active-defrag-max-scan-fields 1000
            r config set active-defrag-threshold-lower 5
            r config set active-defrag-cycle-min 65
            r config set active-defrag-cycle-max 75
            r config set active-defrag-ignore-bytes 2mb
            r config set maxmemory 0
            r config set list-max-ziplist-size 5 ;# list of 500k items will have 100k quicklist nodes

            # create big keys with 10k items
            set rd [redis_deferring_client]

            set expected_frag 1.7
            # add a mass of list nodes to two lists (allocations are interlaced)
            set val [string repeat A 100] ;# 5 items of 100 bytes puts us in the 640 bytes bin, which has 32 regs, so high potential for fragmentation
            set elements 500000
            for {set j 0} {$j < $elements} {incr j} {
                $rd lpush biglist1 $val
                $rd lpush biglist2 $val
            }
            for {set j 0} {$j < $elements} {incr j} {
                $rd read ; # Discard replies
                $rd read ; # Discard replies
            }

            # create some fragmentation
            r del biglist2

            # start defrag
            after 120 ;# serverCron only updates the info once in 100ms
            set frag [s allocator_frag_ratio]
            if {$::verbose} {
                puts "frag $frag"
            }

            assert {$frag >= $expected_frag}
            r config set latency-monitor-threshold 5
            r latency reset

            set digest [debug_digest]
            catch {r config set activedefrag yes} e
            if {[r config get activedefrag] eq "activedefrag yes"} {
                # wait for the active defrag to start working (decision once a second)
                wait_for_condition 50 100 {
                    [s total_active_defrag_time] ne 0
                } else {
                    after 120 ;# serverCron only updates the info once in 100ms
                    puts [r info memory]
                    puts [r info stats]
                    puts [r memory malloc-stats]
                    fail "defrag not started."
                }

                # wait for the active defrag to stop working
                wait_for_condition 500 100 {
                    [s active_defrag_running] eq 0
                } else {
                    after 120 ;# serverCron only updates the info once in 100ms
                    puts [r info memory]
                    puts [r info stats]
                    puts [r memory malloc-stats]
                    fail "defrag didn't stop."
                }

                # test the fragmentation is lower
                after 120 ;# serverCron only updates the info once in 100ms
                set misses [s active_defrag_misses]
                set hits [s active_defrag_hits]
                set frag [s allocator_frag_ratio]
                set max_latency 0
                foreach event [r latency latest] {
                    lassign $event eventname time latency max
                    if {$eventname == "active-defrag-cycle"} {
                        set max_latency $max
                    }
                }
                if {$::verbose} {
                    puts "frag $frag"
                    puts "misses: $misses"
                    puts "hits: $hits"
                    puts "max latency $max_latency"
                    puts [r latency latest]
                    puts [r latency history active-defrag-cycle]
                }
                assert {$frag < 1.1}
                # due to high fragmentation, 100hz, and active-defrag-cycle-max set to 75,
                # we expect max latency to be not much higher than 7.5ms but due to rare slowness threshold is set higher
                if {!$::no_latency} {
                    assert {$max_latency <= 30}
                }

                # in extreme cases of stagnation, we see over 20m misses before the tests aborts with "defrag didn't stop",
                # in normal cases we only see 100k misses out of 500k elements
                assert {$misses < $elements}
            }
            # verify the data isn't corrupted or changed
            set newdigest [debug_digest]
            assert {$digest eq $newdigest}
            r save ;# saving an rdb iterates over all the data / pointers
            r del biglist1 ;# coverage for quicklistBookmarksClear
        } {1}

        test "Active defrag edge case: $type" {
            # there was an edge case in defrag where all the slabs of a certain bin are exact the same
            # % utilization, with the exception of the current slab from which new allocations are made
            # if the current slab is lower in utilization the defragger would have ended up in stagnation,
            # kept running and not move any allocation.
            # this test is more consistent on a fresh server with no history
            start_server {tags {"defrag"} overrides {save ""}} {
                r flushdb
                r config resetstat
                r config set hz 100
                r config set activedefrag no
                r config set active-defrag-max-scan-fields 1000
                r config set active-defrag-threshold-lower 5
                r config set active-defrag-cycle-min 65
                r config set active-defrag-cycle-max 75
                r config set active-defrag-ignore-bytes 1mb
                r config set maxmemory 0
                set expected_frag 1.3

                r debug mallctl-str thread.tcache.flush VOID
                # fill the first slab containing 32 regs of 640 bytes.
                for {set j 0} {$j < 32} {incr j} {
                    r setrange "_$j" 600 x
                    r debug mallctl-str thread.tcache.flush VOID
                }

                # add a mass of keys with 600 bytes values, fill the bin of 640 bytes which has 32 regs per slab.
                set rd [redis_deferring_client]
                set keys 640000
                for {set j 0} {$j < $keys} {incr j} {
                    $rd setrange $j 600 x
                }
                for {set j 0} {$j < $keys} {incr j} {
                    $rd read ; # Discard replies
                }

                # create some fragmentation of 50%
                set sent 0
                for {set j 0} {$j < $keys} {incr j 1} {
                    $rd del $j
                    incr sent
                    incr j 1
                }
                for {set j 0} {$j < $sent} {incr j} {
                    $rd read ; # Discard replies
                }

                # create higher fragmentation in the first slab
                for {set j 10} {$j < 32} {incr j} {
                    r del "_$j"
                }

                # start defrag
                after 120 ;# serverCron only updates the info once in 100ms
                set frag [s allocator_frag_ratio]
                if {$::verbose} {
                    puts "frag $frag"
                }

                assert {$frag >= $expected_frag}

                set digest [debug_digest]
                catch {r config set activedefrag yes} e
                if {[r config get activedefrag] eq "activedefrag yes"} {
                    # wait for the active defrag to start working (decision once a second)
                    wait_for_condition 50 100 {
                        [s total_active_defrag_time] ne 0
                    } else {
                        after 120 ;# serverCron only updates the info once in 100ms
                        puts [r info memory]
                        puts [r info stats]
                        puts [r memory malloc-stats]
                        fail "defrag not started."
                    }

                    # wait for the active defrag to stop working
                    wait_for_condition 500 100 {
                        [s active_defrag_running] eq 0
                    } else {
                        after 120 ;# serverCron only updates the info once in 100ms
                        puts [r info memory]
                        puts [r info stats]
                        puts [r memory malloc-stats]
                        fail "defrag didn't stop."
                    }

                    # test the fragmentation is lower
                    after 120 ;# serverCron only updates the info once in 100ms
                    set misses [s active_defrag_misses]
                    set hits [s active_defrag_hits]
                    set frag [s allocator_frag_ratio]
                    if {$::verbose} {
                        puts "frag $frag"
                        puts "hits: $hits"
                        puts "misses: $misses"
                    }
                    assert {$frag < 1.1}
                    assert {$misses < 10000000} ;# when defrag doesn't stop, we have some 30m misses, when it does, we have 2m misses
                }

                # verify the data isn't corrupted or changed
                set newdigest [debug_digest]
                assert {$digest eq $newdigest}
                r save ;# saving an rdb iterates over all the data / pointers
            }
        } ;# standalone
        }
    }
    }

    start_cluster 1 0 {tags {"defrag external:skip cluster"} overrides {appendonly yes auto-aof-rewrite-percentage 0 save ""}} {
        test_active_defrag "cluster"
    }

    start_server {tags {"defrag external:skip standalone"} overrides {appendonly yes auto-aof-rewrite-percentage 0 save ""}} {
        test_active_defrag "standalone"
    }
} ;# run_solo
