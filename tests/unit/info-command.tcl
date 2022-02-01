start_server {tags {"info and its relative command"}} {
    test "info command with only one argument" {
        set info [r info]
        assert { [string match "*redis_version*" $info] }
        assert { [string match "*maxclients*" $info] }
        assert { [string match "*used_cpu_user*" $info] }
        assert { ![string match "*sentinel_tilt*" $info] }
        assert { [string match "*used_memory*" $info] }
        assert { [string match "*rdb_last_bgsave*" $info] }
        assert { [string match "*master_repl_offset*" $info] }
        assert { [string match "*cluster_enabled*" $info] }
        assert { ![string match "*rejected_calls*" $info] }        
    }

    test "info all command" {
        set info [r info all]
        assert { [string match "*redis_version*" $info] }
        assert { [string match "*maxclients*" $info] }
        assert { [string match "*used_cpu_user*" $info] }
        assert { ![string match "*sentinel_tilt*" $info] }
        assert { [string match "*used_memory*" $info] }
        assert { [string match "*rdb_last_bgsave*" $info] }
        assert { [string match "*master_repl_offset*" $info] }
        assert { [string match "*cluster_enabled*" $info] }
        assert { [string match "*rejected_calls*" $info] }
    }

    test "info default command" {
        set info [r info default]
        assert { [string match "*redis_version*" $info] }
        assert { [string match "*maxclients*" $info] }
        assert { [string match "*used_cpu_user*" $info] }
        assert { ![string match "*sentinel_tilt*" $info] }
        assert { [string match "*used_memory*" $info] }
        assert { [string match "*rdb_last_bgsave*" $info] }
        assert { [string match "*master_repl_offset*" $info] }
        assert { [string match "*cluster_enabled*" $info] }
        assert { ![string match "*rejected_calls*" $info] }
    }

    test "info everything command" {
        set info [r info everything]
        assert { [string match "*redis_version*" $info] }
        assert { [string match "*maxclients*" $info] }
        assert { [string match "*used_cpu_user*" $info] }
        assert { ![string match "*sentinel_tilt*" $info] }
        assert { [string match "*used_memory*" $info] }
        assert { [string match "*rdb_last_bgsave*" $info] }
        assert { [string match "*master_repl_offset*" $info] }
        assert { [string match "*cluster_enabled*" $info] }
        assert { [string match "*rejected_calls*" $info] }
    }

    test "info command with one sub-section" {
        set info [r info cpu]
        assert { [string match "*used_cpu_user*" $info] }
        assert { ![string match "*sentinel_tilt*" $info] }
        assert { ![string match "*master_repl_offset*" $info] }
        assert { ![string match "*cluster_enabled*" $info] }

        set info [r info sentinel]
        assert { ![string match "*sentinel_tilt*" $info] }
        assert { ![string match "*used_memory*" $info] }
        assert { ![string match "*rdb_last_bgsave*" $info] }
        assert { ![string match "*used_cpu_user*" $info] }

        set info [r info replication]
        assert { ![string match "*used_cpu_user*" $info] }
        assert { ![string match "*sentinel_tilt*" $info] }
        assert { ![string match "*used_memory*" $info] }
        assert { [string match "*master_repl_offset*" $info] }

        set info [r info server]
        assert { [string match "*redis_version*" $info] }
        assert { ![string match "*maxclients*" $info] }
        assert { ![string match "*used_cpu_user*" $info] }
        assert { ![string match "*sentinel_tilt*" $info] }
        assert { ![string match "*used_memory*" $info] }
    }

    test "info command with multiple sub-sections" {
        set info [r info cpu sentinel]
        assert { [string match "*used_cpu_user*" $info] }
        assert { ![string match "*sentinel_tilt*" $info] }
        assert { ![string match "*master_repl_offset*" $info] }
        assert { ![string match "*cluster_enabled*" $info] }

        set info [r info cpu all]
        assert { [string match "*used_cpu_user*" $info] }
        assert { ![string match "*sentinel_tilt*" $info] }
        assert { [string match "*used_memory*" $info] }
        assert { [string match "*master_repl_offset*" $info] }
        assert { [string match "*rejected_calls*" $info] }

        set info [r info server cpu replication]
        assert { [string match "*redis_version*" $info] }
        assert { ![string match "*maxclients*" $info] }
        assert { [string match "*used_cpu_user*" $info] }
        assert { ![string match "*sentinel_tilt*" $info] }
        assert { ![string match "*used_memory*" $info] }
        assert { [string match "*master_repl_offset*" $info] }
    }

   
}
