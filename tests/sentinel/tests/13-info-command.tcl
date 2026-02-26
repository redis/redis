source "../tests/includes/init-tests.tcl"

test "info command with at most one argument" {
    set subCommandList {}
    foreach arg {"" "all" "default" "everything"} {
        if {$arg == ""} {
            set info [S 0 info]
        } else {
            set info [S 0 info $arg]
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
    set info [S 0 info cpu]
    assert { [string match "*used_cpu_user*" $info] }
    assert { ![string match "*sentinel_tilt*" $info] }
    assert { ![string match "*redis_version*" $info] }

    set info [S 0 info sentinel]
    assert { [string match "*sentinel_tilt*" $info] }
    assert { ![string match "*used_cpu_user*" $info] }
    assert { ![string match "*redis_version*" $info] }
}

test "info command with multiple sub-sections" {
    set info [S 0 info server sentinel replication]
    assert { [string match "*redis_version*" $info] }
    assert { [string match "*sentinel_tilt*" $info] }
    assert { ![string match "*used_memory*" $info] }
    assert { ![string match "*used_cpu_user*" $info] }

    set info [S 0 info cpu all]
    assert { [string match "*used_cpu_user*" $info] }
    assert { [string match "*sentinel_tilt*" $info] }
    assert { [string match "*redis_version*" $info] }
    assert { ![string match "*used_memory*" $info] }
    assert { ![string match "*master_repl_offset*" $info] }
}
