# Every time a master goes down and sentinels promote a slave to
# be the new master, sentinels wait for a while (publish-period * 4,
# that's 8s by default) before sending slaveof command in a +convert-to-slave
# event to force the rebooted master to reconfigure as slave.
proc wait_for_master_reconfigured_as_slave { instance_id master_name err_msg} {
    wait_for_condition 200 50 {
        [RI $instance_id "master_port"] == [lindex [S 0 SENTINEL GET-MASTER-ADDR-BY-NAME $master_name] 1]
    } else {
        fail $err_msg
    }
}

proc restart_killed_instances {} {
    foreach type {redis sentinel} {
        foreach_${type}_id id {
            if {[get_instance_attrib $type $id pid] == -1} {
                puts -nonewline "$type/$id "
                flush stdout
                restart_instance $type $id
            }
        }
    }
}

proc verify_sentinel_auto_discovery { {master_name {}} } {
    if {$master_name eq {}} {
        set master_name "mymaster"
    }

    set sentinels [llength $::sentinel_instances]
    foreach_sentinel_id id {
        wait_for_condition 1000 50 {
            [dict get [S $id SENTINEL MASTER $master_name] num-other-sentinels] == ($sentinels-1)
        } else {
            fail "For master $master_name, at least some sentinel can't detect some other sentinel"
        }
    }
}