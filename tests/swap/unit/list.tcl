start_server {tags {"list"} overrides {save ""}} {
    start_server {overrides {save ""}} {
        set master_host [srv 0 host]
        set master_port [srv 0 port]
        set master [srv 0 client]
        set slave_host [srv -1 host]
        set slave_port [srv -1 port]
        set slave [srv -1 client]

        $slave slaveof $master_host $master_port
        wait_for_sync $slave

        test {swap-list chaos} {
            set rounds 5
            set loaders 5
            set duration 30
            set lists 4; # NOTE: keep it equal to lists in run load below

            for {set round 0} {$round < $rounds} {incr round} {
                puts "chaos load $lists lists with $loaders loaders in $duration seconds ($round/$rounds)"

                # load with chaos list operations
                for {set loader 0} {$loader < $loaders} {incr loader} {
                    lappend load_handles [start_run_load $master_host $master_port $duration 0 {
                        set lists 4
                        set list_max_length 1000
                        set block_timeout 0.1
                        set loader_id [pid]
                        set trim_shink_max 4

                        set mylist "mylist-[randomInt $lists]"
                        set mylist_len [$r1 llen $mylist]

                        if {$mylist_len < $trim_shink_max} {
                            set trim_shink $mylist_len
                        } else {
                            set trim_shink $trim_shink_max
                        }

                        set mylist_start [randomInt $trim_shink] 
                        set mylist_stop [expr $mylist_len - [randomInt $trim_shink]]
                        set otherlist "mylist-[randomInt $lists]"
                        set src_direction [randpath {return LEFT} {return RIGHT}]
                        set dst_direction [randpath {return LEFT} {return RIGHT}]

                        randpath {
                            # head & tail (lpush/lpop/rpush/rpop/lmove...)
                            $r1 LPUSH $mylist $loader_id $loader_id $loader_id $loader_id
                        } {
                            $r1 LPOP $mylist 
                        } {
                            $r1 RPUSH $mylist $loader_id $loader_id $loader_id $loader_id
                        } {
                            $r1 RPOP $mylist
                        } {
                            $r1 RPOPLPUSH $mylist $otherlist
                        } {
                            $r1 LMOVE $mylist $otherlist $src_direction $dst_direction
                        } {
                            # middle (lset/lindex)
                            $r1 LINDEX $mylist [randomInt $mylist_len]
                        } {
                            catch { $r1 LSET $mylist [randomInt $mylist_len] $loader_id } e
                            if {$e != "OK" && ![string match {*no such key*} $e] && ![string match {*index out of range*} $e] } {
                                error "unexpected: $e"
                            }
                        } {
                            # range (lrange/ltrim)
                            $r1 LRANGE $mylist_len [randomInt $mylist_len] [randomInt $mylist_len]
                        } {
                            $r1 LTRIM $mylist $mylist_start $mylist_stop
                        } {
                            # all (linsert/lpos/lrem)
                            $r1 LINSERT $mylist BEFORE $loader_id $loader_id
                        } {
                            $r1 LPOS $mylist $loader_id
                        } {
                            $r1 LREM $mylist 1 $loader_id
                        } {
                            # block (blpop/brpop/brpoplpush/blmove/blmpop...)
                            $r1 BRPOP $mylist $otherlist $block_timeout
                        } {
                            $r1 BLPOP $mylist $otherlist $block_timeout
                        } {
                            $r1 BRPOPLPUSH $mylist $otherlist $block_timeout
                        } {
                            $r1 BLMOVE $mylist $otherlist $src_direction $dst_direction $block_timeout
                        }
                        $r1 LTRIM $mylist 0 $list_max_length


                    }]

                }

                after [expr $duration*1000]
                wait_load_handlers_disconnected

                wait_for_ofs_sync $master $slave

                # save to check list meta consistency
                $master save
                $slave save
                verify_log_message 0 "*DB saved on disk*" 0
                verify_log_message -1 "*DB saved on disk*" 0

                # digest to check master slave consistency
                for {set keyidx 0} {$keyidx < $lists} {incr keyidx} {
                    set master_digest [$master debug digest-value mylist-$keyidx]
                    set slave_digest [$slave debug digest-value mylist-$keyidx]
                    assert_equal $master_digest $slave_digest
                }
            }
        }
    }
}

start_server {tags {"target"}} {
    test "target" {
        r config set swap-debug-evict-keys 0
        r lpush target a 
        set rd1 [redis_deferring_client]
        $rd1 brpoplpush src target 0 
        # r evict target 
        wait_for_condition 10 200 {
            [r swap.evict target] == 1
        } else {
            fail "swap.evict target fail"
        }
        wait_key_cold r target
        r lpush src b
        assert_equal "b a" [r lrange target 0 -1]
    }

    test "lrange - warm list" {
        r config set swap-debug-evict-keys 0
        r rpush test_list v1 v2 v3 v4
        r swap.evict test_list
        wait_key_cold r test_list
        r lset test_list 0 v11
        assert_match [r lrange test_list 0 -1] {v11 v2 v3 v4}
    }
}

start_server {tags {"repl"}} {
    r config set swap-debug-evict-keys 0
    r config set swap-debug-rio-delay-micro 100000
    test " lpush + (swap start) + del + (swap end) + lpush " {
        r rpush target a 
        set rd1 [redis_deferring_client]
        $rd1 brpoplpush src target 0 
        wait_for_condition 10 200 {
            [r swap.evict target] == 1
        } else {
            fail "swap.evict target fail"
        }
        wait_key_cold r target
        set rd2 [redis_deferring_client]
        $rd2 lpush src b 
        r del src 
        $rd2 lpush src a 
        assert_equal [$rd1 read] a
    }

    test " lpush + (swap start) + del + (change type) + (swap end) + del + lpush " {
        r rpush target a 
        set rd1 [redis_deferring_client]
        $rd1 brpoplpush src target 0 
        wait_for_condition 10 200 {
            [r swap.evict target] == 1
        } else {
            fail "swap.evict target fail"
        }
        wait_key_cold r target
        set rd2 [redis_deferring_client]
        $rd2 lpush src b 
        r del src 
        $rd2 set src v 
        r del src 
        $rd2 lpush src a 
        
        assert_equal [$rd1 read] a
    }
    
}

start_server {tags {"repl"}} {
    test " brpoplpush update swapunblock version" {
        assert_equal [status r swap_dependency_block_version] 0
        set rd1 [redis_deferring_client]
        $rd1 brpoplpush src target 5 
        wait_for_condition 100 20 {
            [status r swap_dependency_block_version] == 1
        } else {
            fail "brpoplpush can't update version"
        }
    }
    test " blmove update swapunblock version" {
        assert_equal [status r swap_dependency_block_version] 1
        set rd1 [redis_deferring_client]
        $rd1 blmove src target right left 5
        wait_for_condition 100 20 {
            [status r swap_dependency_block_version] == 2
        } else {
            fail "brpoplpush can't update version"
        }
    }

    test "swapunblock total count status" {
        r config set swap-debug-evict-keys 0
        r rpush target a 
        assert_equal [status r swap_dependency_block_total_count] 0
        set rd1 [redis_deferring_client]
        set rd2 [redis_deferring_client]
        $rd1 brpoplpush src target 0 
        wait_for_condition 10 200 {
            [r swap.evict target] == 1
        } else {
            fail "swap.evict target fail"
        }
        wait_key_cold r target
        r config set swap-debug-rio-delay-micro 100000
        r lpush src b 
        wait_for_condition 10 50 {
            [status r swap_dependency_block_swapping_count] == 1
        } else {
            fail "no swapping"
        }
        # assert_equal 
        # $rd2 brpoplpush a b 5 
        assert_equal [status r swap_dependency_block_total_count] 1
        $rd2 brpoplpush a b 5 
        wait_for_condition 10 50 {
            [status r swap_dependency_block_swapping_count] == 0
        } else {
            fail "unblock fail"
        }
        assert_equal [status r swap_dependency_block_retry_count]  1
    }
}