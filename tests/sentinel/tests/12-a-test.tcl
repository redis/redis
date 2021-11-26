# Check the basic monitoring and failover capabilities.
#source "../tests/includes/start-init-tests.tcl"
source "../tests/includes/init-tests.tcl"


if {$::simulate_error} {
    test "This test will fail" {
        fail "Simulated error"
    }
}


# Restart an instance previously killed by kill_instance
proc restart_instance_local {type id} {

    puts "run restart locally"

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

    # Make sure the instance is not loading the dataset when this
    # function returns.
    # while 1 {
    #    catch {[$link ping]} retval
    #    if {[string match {*LOADING*} $retval]} {
    #        after 100
    #        continue
    #    } else {
    #        break
    #    }
    # }
   
}


test "Before failover output all ids" {
   puts ""
   puts "current master id is $master_id"
   set new_port [RPort $master_id]
   puts "current port is $new_port"
}

test "Restart master test" {
    set old_port [RPort $master_id]
    set addr [S 0 SENTINEL GET-MASTER-ADDR-BY-NAME mymaster]
    assert {[lindex $addr 1] == $old_port}

    puts "populate 10000 keys"
    R $master_id debug populate 10000
    R $master_id bgsave
    R $master_id config set key-load-delay 10000
    R $master_id config set loading-process-events-interval-bytes 1024
    R $master_id config rewrite

    set size_b [R $master_id dbsize]
    puts "now dbsize is $size_b"
    
    foreach_sentinel_id id {
        S $id SENTINEL SET mymaster master-reboot-down-after-period 5000
        S $id sentinel debug ping-period 500
        S $id sentinel debug ask-period 500 
    }

    kill_instance redis $master_id
    set previous_master_id $master_id
   
    restart_instance_local redis $master_id
    
    foreach_sentinel_id id {        
        wait_for_condition 1000 100 {
            [lindex [S $id SENTINEL GET-MASTER-ADDR-BY-NAME mymaster] 1] != $old_port
        } else {
            fail "At least one Sentinel did not receive failover info"
        }
    }
    after 20000   
    puts "current port AAA ---------------------------------------"
    set addr [S 0 SENTINEL GET-MASTER-ADDR-BY-NAME mymaster]
    set master_id [get_instance_id_by_port redis [lindex $addr 1]]
    puts "previous_master_id = $previous_master_id"

}

test "New master [join $addr {:}] role matches" {
    assert {[RI $master_id role] eq {master}}
}

test "after failover output all ids" {
   puts ""
   puts "current master id is $master_id"
   set new_port [RPort $master_id]
   puts "current port is $new_port"
}