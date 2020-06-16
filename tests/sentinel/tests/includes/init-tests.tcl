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

test "(init) Remove old primary entry from sentinels" {
    foreach_sentinel_id id {
        catch {S $id SENTINEL REMOVE myprimary}
    }
}

set redis_slaves 4
test "(init) Create a primary-slaves cluster of [expr $redis_slaves+1] instances" {
    create_redis_primary_slave_cluster [expr {$redis_slaves+1}]
}
set primary_id 0

test "(init) Sentinels can start monitoring a primary" {
    set sentinels [llength $::sentinel_instances]
    set quorum [expr {$sentinels/2+1}]
    foreach_sentinel_id id {
        S $id SENTINEL MONITOR myprimary \
              [get_instance_attrib redis $primary_id host] \
              [get_instance_attrib redis $primary_id port] $quorum
    }
    foreach_sentinel_id id {
        assert {[S $id sentinel primary myprimary] ne {}}
        S $id SENTINEL SET myprimary down-after-milliseconds 2000
        S $id SENTINEL SET myprimary failover-timeout 20000
        S $id SENTINEL SET myprimary parallel-syncs 10
    }
}

test "(init) Sentinels can talk with the primary" {
    foreach_sentinel_id id {
        wait_for_condition 1000 50 {
            [catch {S $id SENTINEL GET-MASTER-ADDR-BY-NAME myprimary}] == 0
        } else {
            fail "Sentinel $id can't talk with the primary."
        }
    }
}

test "(init) Sentinels are able to auto-discover other sentinels" {
    set sentinels [llength $::sentinel_instances]
    foreach_sentinel_id id {
        wait_for_condition 1000 50 {
            [dict get [S $id SENTINEL MASTER myprimary] num-other-sentinels] == ($sentinels-1)
        } else {
            fail "At least some sentinel can't detect some other sentinel"
        }
    }
}

test "(init) Sentinels are able to auto-discover slaves" {
    foreach_sentinel_id id {
        wait_for_condition 1000 50 {
            [dict get [S $id SENTINEL MASTER myprimary] num-slaves] == $redis_slaves
        } else {
            fail "At least some sentinel can't detect some slave"
        }
    }
}
