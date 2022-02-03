# Check the basic monitoring and failover capabilities.
source "../tests/includes/init-tests.tcl"

test "info command with at most one argument" {
    set subCommandList {"" "all" "default" "everything"}
    set info ""
    for {set index 0} {$index < 4} {incr index} {
        if {$index == 0} {
            set info [S 0 info]
        } else {
            set subCommand [lindex $subCommandList $index]
            set info [S 0 info $subCommand]
        }
        assert { [string match "*redis_version*" $info] }
        assert { [string match "*maxclients*" $info] }
        assert { [string match "*used_cpu_user*" $info] }
        assert { [string match "*sentinel_tilt*" $info] }
        assert { ![string match "*used_memory*" $info] }
        assert { ![string match "*rdb_last_bgsave*" $info] }
        assert { ![string match "*master_repl_offset*" $info] }
        assert { ![string match "*cluster_enabled*" $info] }
    }
}

test "info command with one sub-section" {
    set info [S 0 info sentinel]
    assert { [string match "*sentinel_tilt*" $info] }
    assert { ![string match "*used_memory*" $info] }
    assert { ![string match "*rdb_last_bgsave*" $info] }
    assert { ![string match "*used_cpu_user*" $info] }
}


test "info command with multiple sub-sections" {
    set info [S 0 info server sentinel replication]
    assert { [string match "*redis_version*" $info] }
    assert { [string match "*sentinel_tilt*" $info] }
    assert { ![string match "*used_memory*" $info] }
    assert { ![string match "*rdb_last_bgsave*" $info] }
    assert { ![string match "*used_cpu_user*" $info] }

    set info [S 0 info cpu all]
    assert { [string match "*used_cpu_user*" $info] }
    assert { [string match "*sentinel_tilt*" $info] }
    assert { ![string match "*used_memory*" $info] }
    assert { ![string match "*master_repl_offset*" $info] }
}