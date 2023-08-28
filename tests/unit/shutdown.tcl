start_server {tags {"shutdown external:skip"}} {
    test {Temp rdb will be deleted if we use bg_unlink when shutdown} {
        for {set i 0} {$i < 20} {incr i} {
            r set $i $i
        }
        r config set rdb-key-save-delay 10000000

        # Child is dumping rdb
        r bgsave
        wait_for_condition 1000 10 {
            [s rdb_bgsave_in_progress] eq 1
        } else {
            fail "bgsave did not start in time"
        }
        after 100 ;# give the child a bit of time for the file to be created

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

start_server {tags {"shutdown external:skip"} overrides {save {900 1}}} {
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

start_server {tags {"shutdown external:skip"} overrides {save {900 1}}} {
    set pid [s process_id]
    set dump_rdb [file join [lindex [r config get dir] 1] dump.rdb]

    test {RDB save will be failed in shutdown} {
        for {set i 0} {$i < 20} {incr i} {
            r set $i $i
        }

        # create a folder called 'dump.rdb' to trigger temp-rdb rename failure
        # and it will cause rdb save to fail eventually.
        if {[file exists $dump_rdb]} {
            exec rm -f $dump_rdb
        }
        exec mkdir -p $dump_rdb
    }
    test {SHUTDOWN will abort if rdb save failed on signal} {
        # trigger a shutdown which will save an rdb
        exec kill -SIGINT $pid
        wait_for_log_messages 0 {"*Error trying to save the DB, can't exit*"} 0 100 10
    }
    test {SHUTDOWN will abort if rdb save failed on shutdown command} {
        catch {[r shutdown]} err
        assert_match {*Errors trying to SHUTDOWN*} $err
        # make sure the server is still alive
        assert_equal [r ping] {PONG}
    }
    test {SHUTDOWN can proceed if shutdown command was with nosave} {
        catch {[r shutdown nosave]}
        wait_for_log_messages 0 {"*ready to exit, bye bye*"} 0 100 10
    }
    test {Clean up rdb same named folder} {
        exec rm -r $dump_rdb
    }
}


start_server {tags {"shutdown external:skip"} overrides {appendonly no}} {
    test {SHUTDOWN SIGTERM will abort if there's an initial AOFRW - default} {
        r config set shutdown-on-sigterm default
        r config set rdb-key-save-delay 10000000
        for {set i 0} {$i < 10} {incr i} {
            r set $i $i
        }

        r config set appendonly yes
        wait_for_condition 1000 10 {
            [s aof_rewrite_in_progress] eq 1
        } else {
            fail "aof rewrite did not start in time"
        }

        set pid [s process_id]
        exec kill -SIGTERM $pid
        wait_for_log_messages 0 {"*Writing initial AOF, can't exit*"} 0 1000 10

        r config set shutdown-on-sigterm force
    }
}
