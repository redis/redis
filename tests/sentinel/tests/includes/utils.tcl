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