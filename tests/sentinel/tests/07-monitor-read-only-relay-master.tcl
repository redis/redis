# Initialization tests -- most units will start including this.

test "(init) Restart killed instances" {
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

test "(init) Remove old master entry from sentinels" {
    foreach_sentinel_id id {
        catch {S $id SENTINEL REMOVE mymaster}
    }
}

set redis_slaves 2
test "(init) Create a master-relay_masters-slaves cluster of [expr $redis_slaves+4] instances" {
    create_redis_relay_master_slave_cluster [expr {$redis_slaves+4}]
}
set relay_master_id 2
set relay_master_fallback_id 3

test "(init) Sentinels can start monitoring a master" {
    set sentinels [llength $::sentinel_instances]
    set quorum [expr {$sentinels/2+1}]
    foreach_sentinel_id id {
        S $id SENTINEL MONITOR mymaster \
              [get_instance_attrib redis $relay_master_id host] \
              [get_instance_attrib redis $relay_master_id port] $quorum
    }
    foreach_sentinel_id id {
        assert {[S $id sentinel master mymaster] ne {}}
        S $id SENTINEL candidate-slave mymaster \
              [get_instance_attrib redis $relay_master_fallback_id host] \
              [get_instance_attrib redis $relay_master_fallback_id port]
        S $id SENTINEL SET mymaster down-after-milliseconds 2000
        S $id SENTINEL SET mymaster failover-timeout 20000
        S $id SENTINEL SET mymaster parallel-syncs 10
    }
}

test "(init) Sentinels can talk with the master" {
    foreach_sentinel_id id {
        wait_for_condition 1000 50 {
            [catch {S $id SENTINEL GET-MASTER-ADDR-BY-NAME mymaster}] == 0
        } else {
            fail "Sentinel $id can't talk with the master."
        }
    }
}

test "(init) Sentinels are able to auto-discover other sentinels" {
    set sentinels [llength $::sentinel_instances]
    foreach_sentinel_id id {
        wait_for_condition 1000 50 {
            [dict get [S $id SENTINEL MASTER mymaster] num-other-sentinels] == ($sentinels-1)
        } else {
            fail "At least some sentinel can't detect some other sentinel"
        }
    }
}

test "(init) Sentinels are able to auto-discover slaves" {
    foreach_sentinel_id id {
        wait_for_condition 1000 50 {
            [dict get [S $id SENTINEL MASTER mymaster] num-slaves] == $redis_slaves
        } else {
            fail "At least some sentinel can't detect some slave"
        }
    }
}

# Check the basic monitoring and failover capabilities.

test "Basic failover works if the relay master is down" {
    set old_port [RI $relay_master_id tcp_port]
    set addr [S 0 SENTINEL GET-MASTER-ADDR-BY-NAME mymaster]
    assert {[lindex $addr 1] == $old_port}
    assert {[RI $relay_master_fallback_id connected_slaves] == 0}
    kill_instance redis $relay_master_id
    foreach_sentinel_id id {
        wait_for_condition 1000 50 {
            [lindex [S $id SENTINEL GET-MASTER-ADDR-BY-NAME mymaster] 1] != $old_port
        } else {
            fail "At least one Sentinel did not received failover info"
        }
    }
    restart_instance redis $relay_master_id
    set addr [S 0 SENTINEL GET-MASTER-ADDR-BY-NAME mymaster]
}

test "New relay master has all the slaves pointing to it" {
    wait_for_condition 1000 50 {
        [RI $relay_master_fallback_id connected_slaves] == $redis_slaves
    } else {
        fail "Redis ID $relay_master_fallback_id don't have all slaves connected"
    }   
}

test "All the slaves of relay now point to the new relay master" {
    foreach_redis_id id {
        if {$id > 3} {
            wait_for_condition 1000 50 {
                [RI $id master_port] == [lindex $addr 1]
            } else {
                fail "Redis ID $id not configured to replicate with new relay master"
            }
        }
    }
}

test "The old relay master eventually gets reconfigured as a candidate slave" {
    wait_for_condition 1000 50 {
        [lindex [split [dict get [S 0 SENTINEL MASTER mymaster] candidate-slave] :] 1] == $old_port   
    } else {
        fail "Old master not reconfigured as candidate slave of new relay master"
    }
}
