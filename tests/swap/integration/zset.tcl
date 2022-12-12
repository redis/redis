start_server {tags {"zset"} overrides {save ""}} {
    start_server {overrides {save ""}} {
        set master_host [srv 0 host]
        set master_port [srv 0 port]
        set master [srv 0 client]
        set slave_host [srv -1 host]
        set slave_port [srv -1 port]
        set slave [srv -1 client]

        $slave slaveof $master_host $master_port
        wait_for_sync $slave

        puts "$master_host:$master_port"

        test {swap-zset chaos} {
            set rounds 5
            set loaders 5
            set duration 30
            set zsets 4; # NOTE: keep it equal to zsets in run load below

            for {set round 0} {$round < $rounds} {incr round} {
                puts "chaos load $zsets zsets with $loaders loaders in $duration seconds ($round/$rounds)"

                # load with chaos zset operations
                for {set loader 0} {$loader < $loaders} {incr loader} {
                    lappend load_handles [start_run_load $master_host $master_port $duration 0 {
                        set zsets 4
                        set zset_max_length 1000
                        set block_timeout 0.1
                        set loader_id [pid]
                        set trim_shink_max 4

                        set myzset "myzset-[randomInt $zsets]"
                        set myzset_len [$r1 zcard $myzset]

                        if {$myzset_len < $trim_shink_max} {
                            set trim_shink $myzset_len
                        } else {
                            set trim_shink $trim_shink_max
                        }

                        set myzset_start [randomInt $trim_shink] 
                        set myzset_stop [expr $myzset_len - [randomInt $trim_shink]]
                        set otherzset "myzset-[randomInt $zsets]"
                        set src_direction [randpath {return LEFT} {return RIGHT}]
                        set dst_direction [randpath {return LEFT} {return RIGHT}]

                        set d [expr {rand()}]
                        set d2 [expr {rand()}]
                        set v [randomValue]
                        
                        set min [randstring 0 30 alpha]
                        set max [randstring 0 30 alpha]
                        set mininc [randomInt 2]
                        set maxinc [randomInt 2]
                        set block_timeout 0.1
                        if {$mininc} {set cmin "\[$min"} else {set cmin "($min"}
                        if {$maxinc} {set cmax "\[$max"} else {set cmax "($max"}
                        set a [randomInt 10]
                        set b [randomInt 10]
                        if {$a < $b} {
                            set max $b 
                            set min $a 
                        } else {
                            set max $a 
                            set min $b
                        }

                        randpath {
                            # head & tail 
                            $r1 ZADD $myzset $d $v
                        } {
                            $r1 ZINCRBY $myzset $d $v 
                        } {
                            $r1 ZREM $myzset $v
                        } {
                            $r1 ZREVRANK $myzset $v
                        } {
                            $r1 EXPIRE $myzset 5
                        } {
                            $r1 ZREMRANGEBYSCORE $myzset $d +inf
                        } {
                            $r1 ZREMRANGEBYSCORE $myzset -inf $d 
                        } {
                             $r1 ZREMRANGEBYRANK $myzset $min $max
                        } {
                            $r1 ZREMRANGEBYLEX $myzset $cmin $cmax
                        } {
                            $r1 DEL $myzset 
                        } {
                            $r1 ZPOPMAX $myzset [randomInt 10]
                        } {
                            $r1 ZPOPMIN $myzset [randomInt 10]
                        } {
                            $r1 ZCARD  $myzset 
                        } {
                            $r1 ZCOUNT $myzset $min $max
                        } {
                            $r1 ZRANGEBYSCORE $myzset $d +inf
                        } {
                            $r1 ZRANGEBYSCORE $myzset -inf $d 
                        }  {
                            $r1 ZRANGEBYLEX $myzset $cmin $cmax
                        } {
                            $r1 ZREVRANGE  $myzset -2 -1
                        } {
                            $r1 ZREVRANGEBYLEX  $myzset $cmax $cmin
                        } {
                            $r1 ZRANGE $myzset $d +inf BYSCORE
                        } {
                            $r1 ZRANGE $myzset -inf $d  BYSCORE
                        } {
                            $r1 ZRANGE $myzset $cmin $cmax  BYLEX
                        } {
                            $r1 ZRANDMEMBER $myzset -5 
                        }  {
                            $r1 ZSCORE $myzset $v
                        } {
                            $r1 BZPOPMAX $myzset $otherzset $block_timeout
                        } {
                            $r1 BZPOPMIN $myzset $otherzset $block_timeout
                        } {
                            $r1 ZRANGESTORE $myzset $otherzset 2 -1
                        }
                        $r1 ZREMRANGEBYRANK $myzset $zset_max_length -1

                    }]

                }

                after [expr $duration*1000]
                wait_load_handlers_disconnected

                wait_for_ofs_sync $master $slave

                # save to check zset meta consistency
                $master save
                $slave save
                verify_log_message 0 "*DB saved on disk*" 0
                verify_log_message -1 "*DB saved on disk*" 0

                # digest to check master slave consistency
                for {set keyidx 0} {$keyidx < $zsets} {incr keyidx} {
                    set master_digest [$master debug digest-value myzset-$keyidx]
                    set slave_digest [$slave debug digest-value myzset-$keyidx]
                    assert_equal $master_digest $slave_digest
                }
            }
        }
    }
}


