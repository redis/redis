start_server {tags {"shutdown"}} {
    test {Temp rdb will be deleted if we use bg_unlink when shutdown} {
        for {set i 0} {$i < 20} {incr i} {
            r set $i $i
        }
        # It will cost 2s(20 * 100ms) to dump rdb
        r config set rdb-key-save-delay 100000

        # Child is dumping rdb
        r bgsave
        after 100
        set dir [lindex [r config get dir] 1]
        set child_pid [get_child_pid 0]
        set temp_rdb [file join [lindex [r config get dir] 1] temp-${child_pid}.rdb]
        # Temp rdb must be existed
        assert {[file exists $temp_rdb]}

        catch {r shutdown nosave}
        # Make sure the server was killed
        catch {set rd [redis_deferring_client]} e
        assert_match {*connection refused*} $e

        # Temp rdb file must be deleted
        assert {![file exists $temp_rdb]}
    }
}

start_server {tags {"shutdown"}} {
    test {Temp rdb will be deleted in signal handle} {
        for {set i 0} {$i < 20} {incr i} {
            r set $i $i
        }
        # It will cost 2s(20 * 100ms) to dump rdb
        r config set rdb-key-save-delay 100000
        
        set pid [s process_id]
        set temp_rdb [file join [lindex [r config get dir] 1] temp-${pid}.rdb]

        exec kill -SIGINT $pid
        after 100
        # Temp rdb must be existed
        assert {[file exists $temp_rdb]}

        # Temp rdb file must be deleted
        exec kill -SIGINT $pid
        after 100
        assert {![file exists $temp_rdb]}
    }
}
