test "Sentinels aren't monitoring any master" {
    foreach_sentinel_id id {
        assert {[S $id sentinel masters] eq {}}
    }
}

test "Create a master-slaves cluster of 3 instances" {
    create_redis_master_slave_cluster 3
}

test "Sentinels can start monitoring a master" {
    set sentinels [llength $::sentinel_instances]
    set quorum [expr {$sentinels/2+1}]
    foreach_sentinel_id id {
        S $id SENTINEL MONITOR mymaster [get_instance_attrib redis 0 host] \
              [get_instance_attrib redis 0 port] $quorum
    }
    foreach_sentinel_id id {
        assert {[S $id sentinel master mymaster] ne {}}
    }
}

test "Sentinels are able to auto-discover other sentinels" {
    set sentinels [llength $::sentinel_instances]
    foreach_sentinel_id id {
        wait_for_condition 100 50 {
            [dict get [S $id SENTINEL MASTER mymaster] num-other-sentinels] == ($sentinels-1)
        } else {
            fail "At least some sentinel can't detect some other sentinel"
        }
    }
}
