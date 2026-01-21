# Initialization tests -- most units will start including this.
source "../tests/includes/utils.tcl"

proc sentinel_monitor_master { master_name master_id } {
    set sentinels [llength $::sentinel_instances]
    set quorum [expr {$sentinels/2+1}]
    foreach_sentinel_id id {
        S $id SENTINEL MONITOR $master_name \
          [get_instance_attrib redis $master_id host] [get_instance_attrib redis $master_id port] $quorum
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

proc init_cluster { master_name master_id num_instances } {
    puts "Initializing test setup for cluster: master $master_name with $num_instances instances"

    test "(init) Restart killed instances" {
        restart_killed_instances
    }

    test "(init) Remove old master entry from sentinels" {
        foreach_sentinel_id id {
            catch {S $id SENTINEL REMOVE $master_name}
        }
    }

    test "(init) Create a master-slaves cluster of $num_instances instances" {
        create_redis_master_slave_cluster $num_instances $master_id
    }

    test "(init) Sentinels can start monitoring a master" {
        sentinel_monitor_master $master_name $master_id
    }

    test "(init) Sentinels can talk with the master" {
        verify_sentinel_connect_to_master $master_name
    }

    test "(init) Sentinels are able to auto-discover other sentinels" {
        verify_sentinel_auto_discovery $master_name
    }

    test "(init) Sentinels are able to auto-discover slaves" {
        verify_sentinel_discover_slaves $master_name [expr $num_instances - 1]
    }
}

set master_id 0
init_cluster "mymaster" $master_id $::instances_count