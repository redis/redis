# Check the basic monitoring and failover capabilities.
source "../tests/includes/init-tests.tcl"


if {$::simulate_error} {
    test "This test will fail" {
        fail "Simulated error"
    }
}


# Reboot an instance previously in very short time but do not check if it is loading
proc reboot_instance {type id} {
    set dirname "${type}_${id}"
    set cfgfile [file join $dirname $type.conf]
    set port [get_instance_attrib $type $id port]

    # Execute the instance with its old setup and append the new pid
    # file for cleanup.
    set pid [exec_instance $type $dirname $cfgfile]
    set_instance_attrib $type $id pid $pid
    lappend ::pids $pid

    # Check that the instance is running
    if {[server_is_up 127.0.0.1 $port 100] == 0} {
        set logfile [file join $dirname log.txt]
        puts [exec tail $logfile]
        abort_sentinel_test "Problems starting $type #$id: ping timeout, maybe server start failed, check $logfile"
    }

    # Connect with it with a fresh link
    set link [redis 127.0.0.1 $port 0 $::tls]
    $link reconnect 1
    set_instance_attrib $type $id link $link
}


test "Master reboot in very short time" {
    set old_port [RPort $master_id]
    set addr [S 0 SENTINEL GET-MASTER-ADDR-BY-NAME mymaster]
    assert {[lindex $addr 1] == $old_port}
    
    R $master_id debug populate 10000
    R $master_id bgsave
    R $master_id config set key-load-delay 1500
    R $master_id config set loading-process-events-interval-bytes 1024
    R $master_id config rewrite

    foreach_sentinel_id id {
        S $id SENTINEL SET mymaster master-reboot-down-after-period 5000
        S $id sentinel debug ping-period 500
        S $id sentinel debug ask-period 500 
    }

    kill_instance redis $master_id
    reboot_instance redis $master_id
    
    foreach_sentinel_id id {        
        wait_for_condition 1000 100 {
            [lindex [S $id SENTINEL GET-MASTER-ADDR-BY-NAME mymaster] 1] != $old_port
        } else {
            fail "At least one Sentinel did not receive failover info"
        }
    }

    set addr [S 0 SENTINEL GET-MASTER-ADDR-BY-NAME mymaster]
    set master_id [get_instance_id_by_port redis [lindex $addr 1]]

    # Make sure the instance load all the dataset
    while 1 {
        catch {[$link ping]} retval
        if {[string match {*LOADING*} $retval]} {
            after 100
            continue
        } else {
            break
        }
    }
}

test "New master [join $addr {:}] role matches" {
    assert {[RI $master_id role] eq {master}}
}

test "All the other slaves now point to the new master" {
    foreach_redis_id id {
        if {$id != $master_id && $id != 0} {
            wait_for_condition 1000 50 {
                [RI $id master_port] == [lindex $addr 1]
            } else {
                fail "Redis ID $id not configured to replicate with new master"
            }
        }
    }
}

test "The old master eventually gets reconfigured as a slave" {
    wait_for_condition 1000 50 {
        [RI 0 master_port] == [lindex $addr 1]
    } else {
        fail "Old master not reconfigured as slave of new master"
    }
}