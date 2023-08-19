source tests/support/cluster.tcl

# Start a cluster with 3 masters
start_cluster 3 0 {tags {external:skip cluster} overrides {cluster-replica-no-failover yes}} {
    test "Test keys distributed to mutiple nodes are all hit during cluster scan" {
        # Cluster client handles redirection, so fill the cluster with 10000 keys
        set cluster [redis_cluster [srv 0 host]:[srv 0 port]]
        set total_keys 10000
        for {set j 0} {$j < $total_keys} {incr j} {
            $cluster set $j foo
        }
        
        set key_count 0
        set cursor [$cluster cscan "0"]
        while {$cursor != "0"} {
            set result [$cluster cscan $cursor]
            set cursor [lindex $result 0]
            set key_count [expr $key_count + [llength [lindex $result 1]]]
        }
        assert_equal $total_keys $key_count
    }
}