# Test slave selection algorithm.
#
# This unit should test:
# 1) That when there are no suitable slaves no failover is performed.
# 2) That among the available slaves, the one with better offset is picked.

source ../tests/includes/init-tests.tcl

foreach_sentinel_id id {
    S $id SENTINEL DEBUG ping-period 500
    S $id SENTINEL DEBUG ask-period 500
    S $id SENTINEL DEBUG info-period 500
    S $id SENTINEL DEBUG default-down-after 1000
}

# This unit is the only one in the whole sentinel test suite that
# requires two clusters. Here we will mainly operate on the second cluster.

# Spawn 1 master with only 1 slave
set num_instances 2
spawn_instance redis $::redis_base_port $num_instances {
    "enable-protected-configs yes"
    "enable-debug-command yes"
    "save ''"
}

set master_a_id 0
set master_a_name "mymaster"
# The first 5 IDs belong to the default master-slave cluster
set master_b_id $::instances_count
set master_b_name "another_master"
set slave_id [expr $master_b_id + 1]

# Create the second cluster
init_cluster $master_b_name $master_b_id $num_instances

proc change_master { slave_id new_master_id } {
    R $slave_id slaveof [get_instance_attrib redis $new_master_id host] \
                        [get_instance_attrib redis $new_master_id port]
}

# After a slave finishes syncing data with the new master,
# we need to wait for sentinels to fully observe such change.
# At this point, sentinels should have two copies of this slave,
# one under the new cluster and the other under the old cluster,
# because sentinels won't prune any slaves and thus they still
# believe the slave is following the old master.
# So we need to wait for sentinels to see the slave's new replid
# matches with the new master.
proc is_change_master_finished { sentinel_id new_master_name old_master_name } {
    set slave [lindex [S $sentinel_id SENTINEL REPLICAS $old_master_name] 0]
    set slave_master_replid [dict get $slave "master-replid"]
    set new_master_replid [get_info_field [S $sentinel_id SENTINEL INFO-CACHE $new_master_name] "master_replid"]
    return [expr {$new_master_replid eq $slave_master_replid}]
}

proc wait_for_sentinel_confirm_new_master { master_name master_port } {
    foreach_sentinel_id id {
        wait_for_condition 200 100 {
            [lindex [S $id SENTINEL GET-MASTER-ADDR-BY-NAME $master_name] 1] == $master_port
        } else {
            fail "Sentinel $id did not see the new master"
        }
    }
}

test "The second cluster works" {
    # Put a simple string into the database
    R $master_b_id SET "mykey" "myvalue"

    wait_for_condition 100 50 {
        [R $slave_id GET "mykey"] == "myvalue"
    } else {
        fail "The slave and master in the second cluster cannot sync"
    }
}

test "Cannot failover when there's no good slave" {
    set old_port [RPort $master_b_id]

    # This cluster has only one slave. Let's reconfigure the slave to
    # follow the default master instead, so that it will update its
    # replication ID.
    change_master $slave_id $master_a_id

    # The correct order of events is:
    # 1. the slave in the second cluster updates replid after following
    #    the master in the first cluster;
    # 2. sentinels sees the new replid
    # 3. manual failover starts
    # The following wait condition is to strictly guarantee such order.
    foreach_sentinel_id id {
        wait_for_condition 200 50 {
            [is_change_master_finished $id $master_a_name $master_b_name] == 1
        } else {
            fail "Sentinel $id should see the slave has new replid now"
        }
    }

    # The original data should be gone by now
    assert_equal [R $slave_id GET "mykey"] {}

    # We expect the manual failover to fail now that there is no
    # good slave to promote.
    set result [catch { S 0 SENTINEL FAILOVER $master_b_name } err]
    if {$result == 0 || [string match "*NOGOODSLAVE*" $err] == 0} {
        fail "Manual failover did not error with NOGOODSLAVE. Instead, it got: $err"
    }
}

test "Failover should work now that the slave's replication ID is reverted back" {
    # Reconfigure the slave again to bring the slave back to the original
    # master to revert its replication ID.
    change_master $slave_id $master_b_id
    wait_for_condition 100 100 {
        [RI $slave_id master_replid] == [RI $master_b_id master_replid]
    } else {
        fail "Slave couldn't sync with its original master"
    }

    # Since slave b temporarily follows mymaster, sentinels will continue
    # believing it's a slave of the first cluster. We need to reset sentinels' views.
    foreach_sentinel_id id {
        S $id SENTINEL RESET $master_a_name
    }

    # This time the failover should succeed.
    kill_instance redis $master_b_id
    wait_for_sentinel_confirm_new_master $master_b_name [RPort $slave_id]

    # The new master should contain the original data
    assert_equal [R $slave_id GET "mykey"] "myvalue"
}

test "The old master eventually gets reconfigured as a slave" {
    restart_instance redis $master_b_id
    wait_for_master_reconfigured_as_slave $master_b_id $master_b_name "Old master not reconfigured as slave of new master"
}

test "Original master (now slave) gets promoted after the new master (previous slave) goes down" {
    kill_instance redis $slave_id
    wait_for_sentinel_confirm_new_master $master_b_name $old_port

    # The original slave is now slave again
    restart_instance redis $slave_id
    wait_for_master_reconfigured_as_slave $slave_id $master_b_name "The original slave not reconfigured as slave again"
}

# After the master goes down and reboots, it gets assigned a new
# replid different from its slave's. We make sure the failover still
# works in such situation.
test "Slave selection works when the master reboots immediately" {
    # Turn the master into reboot state with a new replid
    kill_instance redis $master_b_id
    restart_instance redis $master_b_id

    # Now crash it to trigger failover process
    kill_instance redis $master_b_id
    wait_for_sentinel_confirm_new_master $master_b_name [RPort $slave_id]
    restart_instance redis $master_b_id
}

# Now that we have two clusters, we need to do proper cleanup
# to avoid messing up other test suites.
foreach_sentinel_id id {
    S $id SENTINEL REMOVE $master_b_name
}
remove_redis_instance [list $master_b_id $slave_id]