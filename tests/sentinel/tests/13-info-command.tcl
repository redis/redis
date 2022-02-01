# Check the basic monitoring and failover capabilities.
source "../tests/includes/init-tests.tcl"


test "info command with only one argument" {
    set info [S 0 info]
    assert { [string match "*redis_version*" $info] }
    assert { [string match "*maxclients*" $info] }
    assert { [string match "*used_cpu_user*" $info] }
    assert { [string match "*sentinel_tilt*" $info] }
    assert { ![string match "*used_memory*" $info] }
    assert { ![string match "*rdb_last_bgsave*" $info] }
    assert { ![string match "*master_repl_offset*" $info] }
    assert { ![string match "*cluster_enabled*" $info] }
}

test "info all command" {
    set info [S 0 info all]
    assert { [string match "*redis_version*" $info] }
    assert { [string match "*maxclients*" $info] }
    assert { [string match "*used_cpu_user*" $info] }
    assert { [string match "*sentinel_tilt*" $info] }
    assert { ![string match "*used_memory*" $info] }
    assert { ![string match "*rdb_last_bgsave*" $info] }
    assert { ![string match "*master_repl_offset*" $info] }
    assert { ![string match "*cluster_enabled*" $info] }
}

test "info default command" {
    set info [S 0 info default]
    assert { [string match "*redis_version*" $info] }
    assert { [string match "*maxclients*" $info] }
    assert { [string match "*used_cpu_user*" $info] }
    assert { [string match "*sentinel_tilt*" $info] }
    assert { ![string match "*used_memory*" $info] }
    assert { ![string match "*rdb_last_bgsave*" $info] }
    assert { ![string match "*master_repl_offset*" $info] }
    assert { ![string match "*cluster_enabled*" $info] }
}

test "info everything command" {
    set info [S 0 info everything]
    assert { [string match "*redis_version*" $info] }
    assert { [string match "*maxclients*" $info] }
    assert { [string match "*used_cpu_user*" $info] }
    assert { [string match "*sentinel_tilt*" $info] }
    assert { ![string match "*used_memory*" $info] }
    assert { ![string match "*rdb_last_bgsave*" $info] }
    assert { ![string match "*master_repl_offset*" $info] }
    assert { ![string match "*cluster_enabled*" $info] }
}

test "info command with one sub-section" {
    set info [S 0 info cpu]
    assert { [string match "*used_cpu_user*" $info] }
    assert { ![string match "*sentinel_tilt*" $info] }
    assert { ![string match "*master_repl_offset*" $info] }
    assert { ![string match "*cluster_enabled*" $info] }

    set info [S 0 info sentinel]
    assert { [string match "*sentinel_tilt*" $info] }
    assert { ![string match "*used_memory*" $info] }
    assert { ![string match "*rdb_last_bgsave*" $info] }
    assert { ![string match "*used_cpu_user*" $info] }

    set info [S 0 info replication]
    assert { ![string match "*used_cpu_user*" $info] }
    assert { ![string match "*sentinel_tilt*" $info] }
    assert { ![string match "*used_memory*" $info] }
    assert { ![string match "*master_repl_offset*" $info] }

    set info [S 0 info server]
    assert { [string match "*redis_version*" $info] }
    assert { ![string match "*maxclients*" $info] }
    assert { ![string match "*used_cpu_user*" $info] }
    assert { ![string match "*sentinel_tilt*" $info] }
    assert { ![string match "*used_memory*" $info] }
}


test "info command with multiple sub-sections" {
    set info [S 0 info cpu sentinel]
    assert { [string match "*used_cpu_user*" $info] }
    assert { [string match "*sentinel_tilt*" $info] }
    assert { ![string match "*master_repl_offset*" $info] }
    assert { ![string match "*cluster_enabled*" $info] }

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

    set info [S 0 info sentinel everything server]
    assert { [string match "*redis_version*" $info] }
    assert { [string match "*maxclients*" $info] }
    assert { [string match "*used_cpu_user*" $info] }
    assert { [string match "*sentinel_tilt*" $info] }
    assert { ![string match "*used_memory*" $info] }
}