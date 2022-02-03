start_server {tags {"info and its relative command"}} {
    test "info command with at most one sub command" {
        set subCommandList {"" "all" "default" "everything"}
        set info ""
        set subCommand ""
        for {set index 0} {$index < 4} {incr index} {
            if {$index == 0} {
                set info [r info]
            } else {
                set subCommand [lindex $subCommandList $index]
                set info [r info $subCommand]
            }
            assert { [string match "*redis_version*" $info] }
            assert { [string match "*used_cpu_user*" $info] }
            assert { ![string match "*sentinel_tilt*" $info] }
            assert { [string match "*used_memory*" $info] }
            if {$subCommand == "" || $subCommand == "default"} {
                assert { ![string match "*rejected_calls*" $info] }        
            } else {
                assert { [string match "*rejected_calls*" $info] }        
            }        
        }
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

        set info [r info commandstats]
        assert { ![string match "*used_cpu_user*" $info] }
        assert { ![string match "*sentinel_tilt*" $info] }
        assert { ![string match "*used_memory*" $info] }
        assert { [string match "*rejected_calls*" $info] }
    }

    test "info command with multiple sub-sections" {
        set info [r info cpu sentinel]
        assert { [string match "*used_cpu_user*" $info] }
        assert { ![string match "*sentinel_tilt*" $info] }
        assert { ![string match "*master_repl_offset*" $info] }

        set info [r info cpu all]
        assert { [string match "*used_cpu_user*" $info] }
        assert { ![string match "*sentinel_tilt*" $info] }
        assert { [string match "*used_memory*" $info] }
        assert { [string match "*master_repl_offset*" $info] }
        assert { [string match "*rejected_calls*" $info] }
        assert { ![string match "*used_cpu_userused_cpu_user*" $info] }

        set info [r info cpu default]
        assert { [string match "*used_cpu_user*" $info] }
        assert { ![string match "*sentinel_tilt*" $info] }
        assert { [string match "*used_memory*" $info] }
        assert { [string match "*master_repl_offset*" $info] }
        assert { ![string match "*rejected_calls*" $info] }
    }

   
}
