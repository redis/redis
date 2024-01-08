source tests/support/benchmark.tcl

proc run_benchmark {cmd} {
    if {[catch { exec {*}$cmd } error]} {
        set first_line [lindex [split $error "\n"] 0]
        puts [colorstr red "redis-benchmark non zero code. first line: $first_line"]
        fail "redis-benchmark non zero code. first line: $first_line"
    }
}

proc cmd_count {cmd_stats} {
    if {[regexp {^calls=([0-9]+?),(.*?)$} $cmd_stats _ value]} {
        set _ $value
    }
}

start_cluster 2 2 {tags {external:skip cluster}} {

    set seed_host [srv 0 host]
    set seed_port [srv 0 port]

    test {Readonly parameter sends requests to primaries and replicas} {
        set cmd [redisbenchmark $seed_host $seed_port "--cluster --readonly -n 120 -t get,set"]
        run_benchmark $cmd

        set total_get_cmds 0
        set total_set_cmds 0

        for { set i 0}  {$i < 4} {incr i} {
            set get_cmd_stats [cmdistat $i get]
            assert_match "*,failed_calls=0" $get_cmd_stats

            set num_get_cmds [cmd_count $get_cmd_stats]
            set total_get_cmds [expr $total_get_cmds + $num_get_cmds]

            # Since redis-benchmark doesn't nessecarily send an equal number of commands to each
            # node in the cluster we need to assert on a range here
            assert {$num_get_cmds > 0}

            set set_cmd_stats [cmdistat $i set]
            assert_match "*,failed_calls=0" $set_cmd_stats

            set num_set_cmds [cmd_count $set_cmd_stats]
            assert {$num_set_cmds > 0}

            if {[getInfoProperty [R $i info replication] role] == "master"} {
                set total_set_cmds [expr $total_set_cmds + $num_set_cmds]
            }
        }

        assert {$total_get_cmds == 120}
        assert {$total_set_cmds == 120}
    }

}
