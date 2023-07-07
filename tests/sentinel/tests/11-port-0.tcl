source "../tests/includes/init-tests.tcl"

test "Start/Stop sentinel on same port with a different runID should not change the total number of sentinels" {
        set sentinel_id [expr $::instances_count-1]
        # Kill sentinel instance
        kill_instance sentinel $sentinel_id

        # Delete line with myid in sentinels config file
        set orgfilename [file join "sentinel_$sentinel_id" "sentinel.conf"]
        set tmpfilename "sentinel.conf_tmp"
        set dirname "sentinel_$sentinel_id"

        delete_lines_with_pattern $orgfilename $tmpfilename "myid"

        # Get count of total sentinels
        set a [S 0 SENTINEL  master mymaster]
        set original_count [lindex $a 33]

        # Restart sentinel with the modified config file
        set pid [exec_instance "sentinel" $dirname $orgfilename]
        lappend ::pids $pid

        after 1000

        # Get new count of total sentinel
        set b [S 0 SENTINEL master mymaster]
        set curr_count [lindex $b 33]

        # If the count is not the same then fail the test
        if {$original_count != $curr_count} {
                fail "Sentinel count is incorrect, original count being $original_count and current count is $curr_count"
        }
}
