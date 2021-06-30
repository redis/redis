# Test PUBLISH propagation across the cluster.

source "../tests/includes/init-tests.tcl"

test "Create a 5 nodes cluster" {
    create_cluster 5 5
}

proc test_cluster_publish {instance instances} {
    # Subscribe all the instances but the one we use to send.
    for {set j 0} {$j < $instances} {incr j} {
        if {$j != $instance} {
            R $j deferred 1
            R $j subscribe testchannel
            R $j read; # Read the subscribe reply
        }
    }

    set data [randomValue]
    R $instance PUBLISH testchannel $data

    # Read the message back from all the nodes.
    for {set j 0} {$j < $instances} {incr j} {
        if {$j != $instance} {
            set msg [R $j read]
            assert {$data eq [lindex $msg 2]}
            R $j unsubscribe testchannel
            R $j read; # Read the unsubscribe reply
            R $j deferred 0
        }
    }
}

test "Test publishing to master" {
    test_cluster_publish 0 10
}

test "Test publishing to slave" {
    test_cluster_publish 5 10
}
