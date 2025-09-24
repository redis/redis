# Initialization tests -- most units will start including this.
source "../tests/includes/utils.tcl"

proc sentinel_monitor_master { master_name master_id } {
    set sentinels [llength $::sentinel_instances]
    set quorum [expr {$sentinels/2+1}]
    foreach_sentinel_id id {
        S $id SENTINEL MONITOR $master_name \
              [get_instance_attrib redis $master_id host] \
              [get_instance_attrib redis $master_id port] $quorum
    }
    foreach_sentinel_id id {
        assert {[S $id sentinel master $master_name] ne {}}
        S $id SENTINEL SET $master_name down-after-milliseconds 2000
        S $id SENTINEL SET $master_name failover-timeout 10000
        S $id SENTINEL debug tilt-period 5000
        S $id SENTINEL SET $master_name parallel-syncs 10
        if {$::leaked_fds_file != "" && [exec uname] == "Linux"} {
            S $id SENTINEL SET $master_name notification-script ../../tests/helpers/check_leaked_fds.tcl
            S $id SENTINEL SET $master_name client-reconfig-script ../../tests/helpers/check_leaked_fds.tcl
        }
    }
}

proc verify_sentinel_connect_to_master { master_name } {
    foreach_sentinel_id id {
        wait_for_condition 1000 50 {
            [catch {S $id SENTINEL GET-MASTER-ADDR-BY-NAME $master_name}] == 0
        } else {
            fail "Sentinel $id can't talk with the master $master_name"
        }
    }
}

proc verify_sentinel_discover_slaves { master_name expected_slaves } {
    foreach_sentinel_id id {
        wait_for_condition 1000 50 {
            [dict get [S $id SENTINEL MASTER $master_name] num-slaves] == $expected_slaves
        } else {
            fail "For master $master_name, at least some sentinel can't detect some slaves"
        }
    }
}

test "(init) Restart killed instances" {
    restart_killed_instances
}

test "(init) Remove old master entry from sentinels" {
    foreach_sentinel_id id {
        catch {S $id SENTINEL REMOVE mymaster}
    }
}

set redis_slaves [expr $::instances_count - 1]
test "(init) Create a master-slaves cluster of [expr $redis_slaves+1] instances" {
    create_redis_master_slave_cluster [expr {$redis_slaves+1}]
}
set master_id 0

test "(init) Sentinels can start monitoring a master" {
    sentinel_monitor_master "mymaster" 0
}

test "(init) Sentinels can talk with the master" {
    verify_sentinel_connect_to_master "mymaster"
}

test "(init) Sentinels are able to auto-discover other sentinels" {
    verify_sentinel_auto_discovery
}

test "(init) Sentinels are able to auto-discover slaves" {
    verify_sentinel_discover_slaves "mymaster" $redis_slaves
}

# Most of the time we only need just one master-slave cluster
if {$::setup_second_master} {
    test "(init) Setting up the second master" {
        # Create 1 master with only 1 slave
        set num_additional_instances 2
        spawn_instance redis $::redis_base_port $num_additional_instances {
            "enable-protected-configs yes"
            "enable-debug-command yes"
            "save ''"
        }
        create_redis_master_slave_cluster $num_additional_instances $::instances_count

        # The first 5 IDs belong to the default master-slave cluster
        set second_master_id 5
        set second_master_name "another_master"

        # Verify the setup
        sentinel_monitor_master $second_master_name $second_master_id
        verify_sentinel_connect_to_master $second_master_name
        verify_sentinel_auto_discovery $second_master_name
        verify_sentinel_discover_slaves $second_master_name [expr $num_additional_instances - 1]
    }
}