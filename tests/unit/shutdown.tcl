start_server {tags {"shutdown external:skip"}} {
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

start_server {tags {"shutdown external:skip"}} {
    test {SHUTDOWN ABORT can cancel SIGTERM} {
        r debug pause-cron 1
        set pid [s process_id]
        exec kill -SIGTERM $pid
        after 10;               # Give signal handler some time to run
        r shutdown abort
        verify_log_message 0 "*Shutdown manually aborted*" 0
        r debug pause-cron 0
        r ping
    } {PONG}

    test {Temp rdb will be deleted in signal handle} {
        for {set i 0} {$i < 20} {incr i} {
            r set $i $i
        }
        # It will cost 2s (20 * 100ms) to dump rdb
        r config set rdb-key-save-delay 100000
        
        set pid [s process_id]
        set temp_rdb [file join [lindex [r config get dir] 1] temp-${pid}.rdb]

        # trigger a shutdown which will save an rdb
        exec kill -SIGINT $pid
        # Wait for creation of temp rdb
        wait_for_condition 50 10 {
            [file exists $temp_rdb]
        } else {
            fail "Can't trigger rdb save on shutdown"
        }

        # Insist on immediate shutdown, temp rdb file must be deleted
        exec kill -SIGINT $pid
        # wait for the rdb file to be deleted
        wait_for_condition 50 10 {
            ![file exists $temp_rdb]
        } else {
            fail "Can't trigger rdb save on shutdown"
        }
    }
}

start_server {tags {"shutdown external:skip"}} {
    set pid [s process_id]
    set temp_rdb [file join [lindex [r config get dir] 1] temp-${pid}.rdb]

    test {Set the rdb file readonly} {
        for {set i 0} {$i < 20} {incr i} {
            r set $i $i
        }

        # set the rdb file to readonly
        if {![file exists $temp_rdb]} {
            exec touch $temp_rdb
        }
        exec chmod 0444 $temp_rdb
    }
    test {SHUTDOWN will abort if rdb save failed on signal} {
        # trigger a shutdown which will save an rdb
        exec kill -SIGINT $pid
        wait_for_log_messages 0 {"*Error trying to save the DB, can't exit*"} 0 100 10
    }
    test {SHUTDOWN will abort if rdb save failed on shutdown command} {
        catch {[r shutdown]} err
        assert_match {*Errors trying to SHUTDOWN*} $err
    }
    test {SHUTDOWN can proceed if shutdown command was with nosave} {
        catch {[r shutdown nosave]}
        wait_for_log_messages 0 {"*ready to exit, bye bye*"} 0 100 10
    }
    test {Reset the rdb file access} {
        exec chmod 0644 $temp_rdb
    }
}
