start_server {tags {"swap.slowlog"} overrides {slowlog-log-slower-than 0}} {
    proc format_command {args} {
        set cmd "*[llength $args]\r\n"
        foreach a $args {
            append cmd "$[string length $a]\r\n$a\r\n"
        }
        set _ $cmd
    }

    test {ctrip.slowlog - check logged entry without traces} {
        r config set swap-debug-trace-latency no
        r set k1 v1
        set e [lindex [r swap.slowlog get] 0]
        assert_equal [llength $e] 8
        assert_equal [lindex $e 3] 1
        assert_equal [expr {[lindex $e 4] > 0}] 1
    }

    test {ctrip.slowlog - check logged entry with traces} {
        r config set swap-debug-trace-latency yes
        r set k1 v1
        set e [lindex [r swap.slowlog get] 0]
        assert_equal [llength $e] 10
        assert_equal [lindex $e 3] 1
        assert_equal [expr {[lindex $e 4] > 0}] 1
        assert_equal {swap traces:} [lindex $e 8]
        assert_equal [string match {IN:lock=[0-9]*,dispatch=[0-9]*,process:[0-9]*,notify:[0-9]*} [lindex $e 9]] 1
    }


    test {ctrip.slowlog - slowlog of accessing hot key} {
        r config set swap-debug-evict-keys 0
        r set k1 v1
        r get k1
        set e [lindex [r swap.slowlog get] 0]
        assert_equal [lindex $e 3] 1
        assert_equal [string match {NOP:lock=[0-9]*,dispatch=-1,process:-1,notify:-1} [lindex $e 9]] 1
    }

    test {ctrip.slowlog - slowlog of accessing cold key} {
        r config set swap-debug-evict-keys 1
        r set k1 v1
        r swap.evict k1
        r get k1
        set e [lindex [r swap.slowlog get] 0]
        assert_equal [lindex $e 3] 1
        assert_equal [string match {IN:lock=[0-9]*,dispatch=[0-9]*,process:[0-9]*,notify:[0-9]*} [lindex $e 9]] 1
    }

    test {ctrip.slowlog - slowlog of accessing unknown key} {
        r del k1
        r get k1
        set e [lindex [r swap.slowlog get] 0]
        assert_equal [lindex $e 3] 1
        assert_equal [string match {NOP:lock=[0-9]*,dispatch=[0-9]*,process:[0-9]*,notify:[0-9]*} [lindex $e 9]] 1
    }

    test {ctrip.slowlog - command with too many swaps requests} {
        r flushall
        r mset k1 v1 k2 v2 k3 v3 k4 v4 k5 v5 k6 v6 k7 v7 k8 v8 k9 v9 k10 v10 k11 v11 k12 v12 k13 v13 k14 v14 k15 v15 k16 v16 k17 v17 k18 v18 k19 v19 k20 v20
        set e [lindex [r swap.slowlog get] 0]
        set traces [lindex $e 9]
        assert_equal [llength $e] 10
        assert_equal [llength $traces] 16
        foreach t $traces {
            assert_equal [string match {NOP:lock=[0-9]*,dispatch=[0-9]*,process:[0-9]*,notify:[0-9]*} $t] 1
        }

        r swap.evict k1 k2 k3 k4 k5 k6 k7 k8 k9 k10 k11 k12 k13 k14 k15 k16 k17 k18 k19 k20
        r mget k1 k2 k3 k4 k5 k6 k7 k8 k9 k10 k11 k12 k13 k14 k15 k16 k17 k18 k19 k20
        set e [lindex [r swap.slowlog get] 0]
        set traces [lindex $e 9]
        assert_equal [llength $traces] 16
        foreach t $traces {
            assert_equal [string match {IN:lock=[0-9]*,dispatch=[0-9]*,process:[0-9]*,notify:[0-9]*} $t] 1
        }
    }

    test {ctrip.slowlog - commands with pipeline} {
        set cmd_cnt 10
        r flushall
        r slowlog reset
        for {set i 0} {$i < $cmd_cnt} {incr i} {
            r write [format_command set k$i $i]
        }
        r flush
        for {set i 0} {$i < $cmd_cnt} {incr i} {
            r read
        }
        set logs [r swap.slowlog get 100]
        assert_equal [llength $logs] 11
        for {set i 0} {$i < $cmd_cnt} {incr i} {
            set j [expr $cmd_cnt - $i - 1]
            set e [lindex $logs $i]
            assert_equal [lindex $e 5] "set k$j $j"
            assert_equal [lindex $e 3] 1
            assert_equal [expr {[lindex $e 4] > 0}] 1
            assert_equal [string match {NOP:lock=[0-9]*,dispatch=[0-9]*,process:[0-9]*,notify:[0-9]*} [lindex $e 9]] 1
        }
    }

    test {ctrip.slowlog - blocked command} {
        set rd [redis_deferring_client]
        $rd setname defer_client
        $rd brpoplpush src dest 0
        r debug sleep 1
        r lpush src a
        set e [lindex [r swap.slowlog get] 0]
        assert_equal [lindex $e 3] 2
        assert_equal [expr {[lindex $e 4] > 0}] 1
    }

    test {ctrip.slowlog - multi/exec} {
        set cmd_cnt 10
        r multi
        for {set i 0} {$i < $cmd_cnt} {incr i} {
            r set k$i $i
        }
        r exec
        set logs [r swap.slowlog get 10]
        for {set i 0} {$i < $cmd_cnt} {incr i} {
            set j [expr $cmd_cnt - $i - 1]
            set e [lindex $logs $i]
            assert_equal [lindex $e 3] 1
            assert_equal [expr {[lindex $e 4] > 0}] 1
            assert_equal [lindex $e 5] "set k$j $j"
        }
    }

    test {ctrip.slowlog - slave slow log} {
        r flushall
        start_server {tags {"swap.slowlog_slave"} overrides {slowlog-log-slower-than 0}} {
            r config set swap-debug-trace-latency yes
            r slaveof [srv -1 host] [srv -1 port]
            wait_for_condition 20 50 {
                [status r master_link_status] eq "up"
            } else {
                fail "replica didn't sync in time"
            }

            set master [srv -1 client]
            $master set k1 v1
            set master_offset [status $master master_repl_offset]
            wait_for_condition 20 50 {
                [lindex [split [string trim [lindex [$master role] 2] "\}"] " "] 2] >= $master_offset
            } else {
                fail "offset didn't sync in time"
            }
            set e [lindex [r swap.slowlog get] 0]
            assert_equal [llength $e] 10
            assert_equal [lindex $e 3] 1
            assert_equal [expr {[lindex $e 4] > 0}] 1
            assert_equal [lindex $e 5] {set k1 v1}
            assert_equal [string match {NOP:lock=[0-9]*,dispatch=[0-9]*,process:[0-9]*,notify:[0-9]*} [lindex $e 9]] 1
        }
    }

}