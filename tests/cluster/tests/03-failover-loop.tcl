# Failover stress test.
# In this test a different node is killed in a loop for N
# iterations. The test checks that certain properties
# are preserved across iterations.

source "../tests/includes/init-tests.tcl"

test "Create a 5 nodes cluster" {
    create_cluster 5 5
}

test "Cluster is up" {
    assert_cluster_state ok
}

set iterations 20
set cluster [redis_cluster 127.0.0.1:[get_instance_attrib redis 0 port]]

while {[incr iterations -1]} {
    set tokill [randomInt 10]
    set other [expr {($tokill+1)%10}] ; # Some other instance.
    set key [randstring 20 20 alpha]
    set val [randstring 20 20 alpha]
    set role [RI $tokill role]
    if {$role eq {master}} {
        set slave {}
        set myid [dict get [get_myself $tokill] id]
        foreach_redis_id id {
            if {$id == $tokill} continue
            if {[dict get [get_myself $id] slaveof] eq $myid} {
                set slave $id
            }
        }
        if {$slave eq {}} {
            fail "Unable to retrieve slave's ID for master #$tokill"
        }
    }

    puts "--- Iteration $iterations ---"

    if {$role eq {master}} {
        test "Wait for slave of #$tokill to sync" {
            wait_for_condition 1000 50 {
                [string match {*state=online*} [RI $tokill slave0]]
            } else {
                fail "Slave of node #$tokill is not ok"
            }
        }
        set slave_config_epoch [CI $slave cluster_my_epoch]
    }

    test "Cluster is writable before failover" {
        for {set i 0} {$i < 100} {incr i} {
            catch {$cluster set $key:$i $val:$i} err
            assert {$err eq {OK}}
        }
        # Wait for the write to propagate to the slave if we
        # are going to kill a master.
        if {$role eq {master}} {
            R $tokill wait 1 20000
        }
    }

    test "Terminating node #$tokill" {
        # Stop AOF so that an initial AOFRW won't prevent the instance from terminating
        R $tokill config set appendonly no
        kill_instance redis $tokill
    }

    if {$role eq {master}} {
        test "Wait failover by #$slave with old epoch $slave_config_epoch" {
            wait_for_condition 1000 50 {
                [CI $slave cluster_my_epoch] > $slave_config_epoch
            } else {
                fail "No failover detected, epoch is still [CI $slave cluster_my_epoch]"
            }
        }
    }

    test "Cluster should eventually be up again" {
        assert_cluster_state ok
    }

    test "Cluster is writable again" {
        for {set i 0} {$i < 100} {incr i} {
            catch {$cluster set $key:$i:2 $val:$i:2} err
            assert {$err eq {OK}}
        }
    }

    test "Restarting node #$tokill" {
        restart_instance redis $tokill
    }

    test "Instance #$tokill is now a slave" {
        wait_for_condition 1000 50 {
            [RI $tokill role] eq {slave}
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

test "Post condition: current_epoch >= my_epoch everywhere" {
    foreach_redis_id id {
        assert {[CI $id cluster_current_epoch] >= [CI $id cluster_my_epoch]}
    }
}
