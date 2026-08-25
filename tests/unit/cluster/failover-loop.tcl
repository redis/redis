# Failover stress test.
# In this test a different node is killed in a loop for N iterations. The test
# checks that certain properties are preserved across iterations.

start_cluster 5 5 {tags {external:skip cluster valgrind:skip} overrides {appendonly yes}} {

test "Cluster is up" {
    wait_for_cluster_state ok
}

set iterations 20
set cluster [redis_cluster 127.0.0.1:[srv 0 port]]

while {[incr iterations -1]} {
    set tokill [randomInt 10]
    set key [randstring 20 20 alpha]
    set val [randstring 20 20 alpha]
    set role [s -$tokill role]
    if {$role eq {master}} {
        set replica {}
        set master_node_id [dict get [cluster_get_myself $tokill] id]
        for {set id 0} {$id < 10} {incr id} {
            if {$id == $tokill} continue
            if {[dict get [cluster_get_myself $id] slaveof] eq $master_node_id} {
                set replica $id
            }
        }
        if {$replica eq {}} {
            fail "Unable to retrieve replica's ID for master #$tokill"
        }
    }

    puts "--- Iteration $iterations ---"

    if {$role eq {master}} {
        test "Wait for slave of #$tokill to sync" {
            wait_for_condition 1000 50 {
                [string match {*state=online*} [s -$tokill slave0]]
            } else {
                fail "Slave of node #$tokill is not ok"
            }
        }
        set replica_config_epoch [CI $replica cluster_my_epoch]
    }

    test "Cluster is writable before failover" {
        for {set i 0} {$i < 100} {incr i} {
            catch {$cluster set $key:$i $val:$i} err
            assert {$err eq {OK}}
        }

        # Wait for the write to propagate to the replica if we are going to
        # kill a master.
        if {$role eq {master}} {
            R $tokill wait 1 20000
        }
    }

    test "Terminating node #$tokill" {
        # Stop AOF so an initial AOFRW cannot delay termination.
        R $tokill config set appendonly no
        cluster_kill_node $tokill
    }

    if {$role eq {master}} {
        test "Wait failover by #$replica with old epoch $replica_config_epoch" {
            wait_for_condition 1000 50 {
                [CI $replica cluster_my_epoch] > $replica_config_epoch
            } else {
                fail "No failover detected, epoch is still [CI $replica cluster_my_epoch]"
            }
        }
    }

    test "Cluster should eventually be up again" {
        wait_for_cluster_state ok [list $tokill]
    }

    test "Cluster is writable again" {
        for {set i 0} {$i < 100} {incr i} {
            catch {$cluster set $key:$i:2 $val:$i:2} err
            assert {$err eq {OK}}
        }
    }

    test "Restarting node #$tokill" {
        cluster_restart_node $tokill
    }

    test "Instance #$tokill is now a slave" {
        wait_for_condition 1000 50 {
            [s -$tokill role] eq {slave}
        } else {
            fail "Restarted instance is not a slave"
        }
    }

    test "We can read back the value we set before" {
        for {set i 0} {$i < 100} {incr i} {
            catch {$cluster get $key:$i} err
            assert {$err eq "$val:$i"}
            catch {$cluster get $key:$i:2} err
            assert {$err eq "$val:$i:2"}
        }
    }
}

$cluster close

test "Post condition: current_epoch >= my_epoch everywhere" {
    for {set id 0} {$id < 10} {incr id} {
        assert {[CI $id cluster_current_epoch] >= [CI $id cluster_my_epoch]}
    }
}

} ;# start_cluster
