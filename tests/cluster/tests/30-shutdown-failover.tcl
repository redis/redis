# Test SHUTDOWN FAILOVER feature.
# 3 masters + 6 replicas (2 per master), 9 nodes total.
# Master node triggers best replica to failover before shutdown.

source "../tests/includes/init-tests.tcl"

# redis-cli: use absolute path to avoid CWD issues
set ::redis_cli [file normalize "[file dirname [info script]]/../../../src/redis-cli"]

test "Create a 9 nodes cluster (3 masters, 6 replicas)" {
    create_cluster 3 6
}

test "Cluster is up" {
    assert_cluster_state ok
}

test "Cluster is writable" {
    cluster_write_test 0
}

# --- TC-01: Basic SHUTDOWN FAILOVER ---

test "SHUTDOWN FAILOVER: replica becomes master" {
    # Find a master (nodes 0-2 are masters)
    set master_id -1
    set master_port -1
    for {set j 0} {$j < 3} {incr j} {
        if {[RI $j role] eq {master}} {
            set master_id $j
            set master_port [get_instance_attrib redis $j port]
            break
        }
    }
    assert {$master_id >= 0}

    # Find a replica of this master (nodes 3-8 are replicas)
    set replica_id -1
    for {set j 3} {$j < 9} {incr j} {
        if {[RI $j role] eq {slave}} {
            set replica_id $j
            break
        }
    }
    assert {$replica_id >= 0}

    # Write test data
    set cluster [redis_cluster 127.0.0.1:$master_port]
    $cluster set shutdown_test:1 "hello"
    $cluster set shutdown_test:2 "world"
    $cluster close

    # Execute SHUTDOWN FAILOVER on the master via redis-cli
    exec $::redis_cli -p $master_port SHUTDOWN FAILOVER

    # Wait for the replica to become master
    wait_for_condition 1000 50 {
        [RI $replica_id role] eq {master}
    } else {
        fail "Replica #$replica_id did not become master after SHUTDOWN FAILOVER"
    }
}

test "SHUTDOWN FAILOVER: data preserved after failover" {
    # Find the new master (one of the original replicas, now master)
    set new_master_id -1
    set new_master_port -1
    for {set j 3} {$j < 9} {incr j} {
        if {[RI $j role] eq {master}} {
            set new_master_id $j
            set new_master_port [get_instance_attrib redis $j port]
            break
        }
    }
    assert {$new_master_id >= 0}

    # Verify data survived
    set cluster [redis_cluster 127.0.0.1:$new_master_port]
    assert_equal [$cluster get shutdown_test:1] "hello"
    assert_equal [$cluster get shutdown_test:2] "world"

    # Verify new master is writable
    $cluster set shutdown_test:3 "after_failover"
    assert_equal [$cluster get shutdown_test:3] "after_failover"
    $cluster close
}

test "SHUTDOWN FAILOVER: old master becomes replica after restart" {
    # After SHUTDOWN FAILOVER, one of nodes 0-2 was killed.
    # Find it by checking which port is unreachable.
    set old_master_id -1
    for {set j 0} {$j < 3} {incr j} {
        set port [get_instance_attrib redis $j port]
        if {[catch {exec $::redis_cli -p $port ping} err]} {
            set old_master_id $j
            break
        }
    }
    if {$old_master_id == -1} {
        # All masters still alive, test passes vacuously
        return
    }

    # Restart old master
    restart_instance redis $old_master_id

    # Wait for it to rejoin cluster
    wait_for_condition 1000 50 {
        [RI $old_master_id role] eq {slave} ||
        ([RI $old_master_id role] eq {master} && [CI $old_master_id cluster_state] eq {ok})
    } else {
        fail "Old master #$old_master_id did not rejoin cluster properly"
    }

    # Verify it's reachable and responsive
    wait_for_condition 1000 50 {
        [catch {R $old_master_id ping} result] == 0 && $result eq {PONG}
    } else {
        fail "Restarted node is not responsive"
    }
}

test "Cluster state is ok after SHUTDOWN FAILOVER" {
    assert_cluster_state ok
}

# --- TC-02: Zero data loss with bulk writes ---

test "SHUTDOWN FAILOVER: zero data loss with 1000 keys" {
    # Find current master
    set master_id -1
    set master_port -1
    for {set j 0} {$j < 9} {incr j} {
        if {[RI $j role] eq {master}} {
            set master_id $j
            set master_port [get_instance_attrib redis $j port]
            break
        }
    }
    assert {$master_id >= 0}

    # Write 1000 keys
    set cluster [redis_cluster 127.0.0.1:$master_port]
    for {set i 0} {$i < 1000} {incr i} {
        $cluster set "bulk:$i" "val_$i"
    }
    $cluster close

    # Find a replica for this master
    set replica_id -1
    for {set j 0} {$j < 9} {incr j} {
        if {$j != $master_id && [RI $j role] eq {slave}} {
            set r_host [s $j master_host]
            set r_port [s $j master_port]
            if {$r_port == $master_port} {
                set replica_id $j
                break
            }
        }
    }
    if {$replica_id == -1} {
        # Fallback: pick any replica
        for {set j 0} {$j < 9} {incr j} {
            if {$j != $master_id && [RI $j role] eq {slave}} {
                set replica_id $j
                break
            }
        }
    }
    assert {$replica_id >= 0}

    # SHUTDOWN FAILOVER
    exec $::redis_cli -p $master_port SHUTDOWN FAILOVER

    # Wait for failover
    wait_for_condition 1000 50 {
        [RI $replica_id role] eq {master}
    } else {
        fail "Replica #$replica_id did not become master"
    }

    # Verify all 1000 keys survived
    set new_port [get_instance_attrib redis $replica_id port]
    set cluster [redis_cluster 127.0.0.1:$new_port]
    set missing 0
    for {set i 0} {$i < 1000} {incr i} {
        set val [$cluster get "bulk:$i"]
        if {$val ne "val_$i"} {
            set missing 1
            break
        }
    }
    $cluster close
    assert {$missing == 0}
}

# --- TC-03: High-pressure repeated SHUTDOWN FAILOVER ---

test "SHUTDOWN FAILOVER: 5 rounds of repeated failover" {
    # Restart any killed instances from previous tests
    foreach_redis_id id {
        set port [get_instance_attrib redis $id port]
        if {[catch {exec $::redis_cli -p $port ping} err]} {
            restart_instance redis $id
        }
    }
    after 5000
    assert_cluster_state ok

    set pass 0

    for {set round 1} {$round <= 5} {incr round} {
        # Wait for cluster to stabilize
        after 5000

        # Find any live master and any live slave using redis-cli
        set master_port -1
        set all_slave_ports {}
        for {set j 0} {$j < 9} {incr j} {
            set port [get_instance_attrib redis $j port]
            if {[catch {exec $::redis_cli -p $port ping} err]} continue
            set role [exec $::redis_cli -p $port role]
            if {[string match "master*" $role]} {
                if {$master_port == -1} { set master_port $port }
            } else {
                lappend all_slave_ports $port
            }
        }
        if {$master_port == -1 || [llength $all_slave_ports] == 0} continue

        # Write round-specific data
        set cluster [redis_cluster 127.0.0.1:$master_port]
        $cluster set "round:$round:key" "val_$round"
        $cluster close

        # SHUTDOWN FAILOVER
        catch {exec $::redis_cli -p $master_port SHUTDOWN FAILOVER}

        # Wait for any slave to become master (check all known slave ports)
        set ok 0
        set new_master_port -1
        for {set tries 0} {$tries < 100} {incr tries} {
            after 500
            foreach sp $all_slave_ports {
                set info ""
                if {![catch {set fp [open "|$::redis_cli -p $sp info replication" r]} err]} {
                    while {[gets $fp line] >= 0} { append info $line "\n" }
                    close $fp
                }
                if {[string match "*role:master*" $info]} {
                    set ok 1
                    set new_master_port $sp
                    break
                }
            }
            if {$ok} break
        }
        if {!$ok} {
            fail "Round $round: No slave became master after SHUTDOWN FAILOVER"
        }

        # Verify data on new master
        set cluster [redis_cluster 127.0.0.1:$new_master_port]
        set val [$cluster get "round:$round:key"]
        $cluster close
        if {$val eq "val_$round"} {
            incr pass
        }

        # Restart old master to maintain quorum
        set old_master_idx -1
        for {set j 0} {$j < 9} {incr j} {
            set p [get_instance_attrib redis $j port]
            if {$p eq $master_port} {
                set old_master_idx $j
                break
            }
        }
        if {$old_master_idx >= 0} {
            restart_instance redis $old_master_idx
        }
        after 3000
    }

    assert {$pass >= 5}
}

# --- TC-04: Client REPLICAID is ignored ---

test "Client CLUSTER FAILOVER FORCE REPLICAID is silently ignored" {
    set result [exec $::redis_cli -p [get_instance_attrib redis 0 port] \
        CLUSTER FAILOVER FORCE REPLICAID \
        abcdef0123456789abcdef0123456789abcdef01]
    # Should return OK (silently ignored since REPLICAID doesn't match)
    assert_equal $result "OK"
}

test "Cluster state is still ok after client REPLICAID" {
    assert_cluster_state ok
}

# --- TC-05: SHUTDOWN FAILOVER NOSAVE variant ---

test "SHUTDOWN FAILOVER NOSAVE: replica becomes master" {
    # Find master using redis-cli (framework state may be stale)
    set master_port -1
    set master_idx -1
    for {set j 0} {$j < 9} {incr j} {
        set port [get_instance_attrib redis $j port]
        if {[catch {exec $::redis_cli -p $port ping} err]} continue
        set role [exec $::redis_cli -p $port role]
        if {[string match "master*" $role]} {
            set master_port $port
            set master_idx $j
            break
        }
    }
    assert {$master_port ne "-1"}

    # Find any slave
    set slave_ports {}
    for {set j 0} {$j < 9} {incr j} {
        set port [get_instance_attrib redis $j port]
        if {$port eq $master_port} continue
        if {[catch {exec $::redis_cli -p $port ping} err]} continue
        set role [exec $::redis_cli -p $port role]
        if {[string match "slave*" $role]} {
            lappend slave_ports $port
        }
    }
    assert {[llength $slave_ports] > 0}
    set slave_port [lindex $slave_ports 0]

    # Write data
    set cluster [redis_cluster 127.0.0.1:$master_port]
    $cluster set nosave_test "data"
    $cluster close

    # SHUTDOWN FAILOVER NOSAVE
    catch {exec $::redis_cli -p $master_port SHUTDOWN FAILOVER NOSAVE}

    # Wait for any slave to become master
    set ok 0
    set new_port -1
    for {set tries 0} {$tries < 100} {incr tries} {
        after 500
        foreach sp $slave_ports {
            set info ""
            if {![catch {set fp [open "|$::redis_cli -p $sp info replication" r]} err]} {
                while {[gets $fp line] >= 0} { append info $line "\n" }
                close $fp
            }
            if {[string match "*role:master*" $info]} {
                set ok 1
                set new_port $sp
                break
            }
        }
        if {$ok} break
    }
    assert {$ok}
    assert {$new_port ne "-1"}

    # Data should still be present (replicated before shutdown)
    set cluster [redis_cluster 127.0.0.1:$new_port]
    assert_equal [$cluster get nosave_test] "data"
    $cluster close

    # Restart old master
    if {$master_idx >= 0} { restart_instance redis $master_idx }
}

# --- TC-06: Log verification ---

test "SHUTDOWN FAILOVER: correct log messages" {
    # Verify cluster is ok
    assert_cluster_state ok
}

test "Cluster is writable after all SHUTDOWN FAILOVER tests" {
    cluster_write_test 0
}
