start_server {overrides {save ""}} {
    set master_host [srv 0 host]
    set master_port [srv 0 port]
    set master [srv 0 client]

    test {swap-lock txid int overflow} {
        r debug set-swap-txid 2147483500

        set num 100
        for {set i 0} {$i < $num} {incr i} {
            set rds($i) [redis_deferring_client]
        }
        for {set i 0} {$i < $num} {incr i} {
            $rds($i) get not-existing-key
        }
        for {set i 0} {$i < $num} {incr i} {
            $rds($i) read
            $rds($i) close
        }
    }

    test {swap-lock chaos} {
        set rounds 5
        set loaders 10
        set duration 30

        for {set round 0} {$round < $rounds} {incr round} {
            puts "chaos test lock with $loaders loaders in $duration seconds ($round/$rounds)"

            for {set loader 0} {$loader < $loaders} {incr loader} {
                lappend load_handles [start_run_load $master_host $master_port $duration 0 {
                    # puts -nonewline .
                    $r1 SELECT 0
                    randpath {
                        $r1 MULTI
                        $r1 SELECT 0
                        $r1 SET key db0
                        $r1 SELECT 1
                        $r1 SET key db1-0
                        $r1 SELECT 0
                        $r1 FLUSHDB
                        $r1 SELECT 1
                        $r1 FLUSHDB
                        $r1 SET key db1-1
                        $r1 FLUSHALL
                        $r1 SELECT 0
                        $r1 SET key db0-2
                        $r1 SELECT 1
                        $r1 SET key db1-2
                        $r1 SELECT 0
                        $r1 EXEC
                    } {
                        $r1 SET key2 db0-3
                    } {
                        $r1 GET key2
                    } {
                        $r1 MSET key db0-4 key2 db0-4
                    } {
                        $r1 MGET key key2
                    } {
                        $r1 SELECT 0
                        $r1 FLUSHDB
                        $r1 SELECT 1
                        $r1 FLUSHDB
                        $r1 SELECT 0
                    } {
                        catch { $r1 BGSAVE }
                    }
                }]
            }

            after [expr $duration*1000]

            wait_load_handlers_disconnected

            waitForBgsave $master
        }
    }
}


