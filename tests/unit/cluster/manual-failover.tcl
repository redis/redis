# Check the manual failover
start_cluster 5 5 {tags {external:skip cluster}} {

test "Cluster is up" {
    wait_for_cluster_state ok
}

test "Cluster is writable" {
    cluster_write_test [srv 0 port]
}

test "Instance #5 is a slave" {
    assert {[s -5 role] eq {slave}}
}

test "Instance #5 synced with the master" {
    wait_for_condition 1000 50 {
        [s -5 master_link_status] eq {up}
    } else {
        fail "Instance #5 master link status is not up"
    }
}

set current_epoch [CI 1 cluster_current_epoch]

set numkeys 50000
set numops 10000
set cluster [redis_cluster 127.0.0.1:[srv 0 port]]
catch {unset content}
array set content {}

test "Send CLUSTER FAILOVER to #5, during load" {
    for {set j 0} {$j < $numops} {incr j} {
        # Write random data to random list.
        set listid [randomInt $numkeys]
        set key "key:$listid"
        set ele [randomValue]
        # We write both with Lua scripts and with plain commands.
        # This way we are able to stress Lua -> Redis command invocation
        # as well, that has tests to prevent Lua to write into wrong
        # hash slots.
        if {$listid % 2} {
            $cluster rpush $key $ele
        } else {
           $cluster eval {redis.call("rpush",KEYS[1],ARGV[1])} 1 $key $ele
        }
        lappend content($key) $ele

        if {($j % 1000) == 0} {
            puts -nonewline W; flush stdout
        }

        if {$j == $numops/2} {R 5 cluster failover}
    }
}

test "Wait for failover" {
    wait_for_condition 1000 50 {
        [CI 1 cluster_current_epoch] > $current_epoch
    } else {
        fail "No failover detected"
    }
}

test "Cluster should eventually be up again" {
    wait_for_cluster_state ok
}

test "Cluster is writable" {
    cluster_write_test [srv -1 port]
}

test "Instance #5 is now a master" {
    assert {[s -5 role] eq {master}}
}

test "Verify $numkeys keys for consistency with logical content" {
    # Check that the Redis Cluster content matches our logical content.
    foreach {key value} [array get content] {
        assert {[$cluster lrange $key 0 -1] eq $value}
    }
}

test "Instance #0 gets converted into a slave" {
    wait_for_condition 1000 50 {
        [s 0 role] eq {slave}
    } else {
        fail "Old master was not converted into slave"
    }
}

} ;# start_cluster

## Check that manual failover does not happen if we can't talk with the master.
start_cluster 5 5 {tags {external:skip cluster}} {

test "Cluster is up" {
    wait_for_cluster_state ok
}

test "Cluster is writable" {
    cluster_write_test [srv 0 port]
}

test "Instance #5 is a slave" {
    assert {[s -5 role] eq {slave}}
}

test "Instance #5 synced with the master" {
    wait_for_condition 1000 50 {
        [s -5 master_link_status] eq {up}
    } else {
        fail "Instance #5 master link status is not up"
    }
}

test "Make instance #0 unreachable without killing it" {
    R 0 deferred 1
    R 0 DEBUG SLEEP 10
}

test "Send CLUSTER FAILOVER to instance #5" {
    R 5 cluster failover
}

test "Instance #5 is still a slave after some time (no failover)" {
    after 5000
    assert {[s -5 role] eq {master}}
}

test "Wait for instance #0 to return back alive" {
    R 0 deferred 0
    assert {[R 0 read] eq {OK}}
}

} ;# start_cluster

## Check with "force" failover happens anyway.
start_cluster 5 10 {tags {external:skip cluster}} {

test "Cluster is up" {
    wait_for_cluster_state ok
}

test "Cluster is writable" {
    cluster_write_test [srv 0 port]
}

test "Instance #5 is a slave" {
    assert {[s -5 role] eq {slave}}
}

test "Instance #5 synced with the master" {
    wait_for_condition 1000 50 {
        [s -5 master_link_status] eq {up}
    } else {
        fail "Instance #5 master link status is not up"
    }
}

test "Make instance #0 unreachable without killing it" {
    R 0 deferred 1
    R 0 DEBUG SLEEP 10
}

test "Send CLUSTER FAILOVER to instance #5" {
    R 5 cluster failover force
}

test "Instance #5 is a master after some time" {
    wait_for_condition 1000 50 {
        [s -5 role] eq {master}
    } else {
        fail "Instance #5 is not a master after some time regardless of FORCE"
    }
}

test "Wait for instance #0 to return back alive" {
    R 0 deferred 0
    assert {[R 0 read] eq {OK}}
}

} ;# start_cluster
