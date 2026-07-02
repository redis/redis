proc backup_status_field {field} {
    array set s [r backup status]
    return $s($field)
}

proc backup_dir_empty {dir} {
    return [expr {[file exists $dir] && [llength [glob -nocomplain -directory $dir *]] == 0}]
}

proc backup_save_rdb_as {path} {
    assert_equal "OK" [r save]
    set dir [lindex [r config get dir] 1]
    file copy -force [file join $dir dump.rdb] $path
}

proc backup_write_local_aof {dir} {
    set aof_dir [file join $dir appendonlydir]
    file mkdir $aof_dir
    set fp [open [file join $aof_dir appendonly.aof.1.incr.aof] w]
    puts -nonewline $fp [formatCommand set local-aof-key should-not-load]
    close $fp

    set fp [open [file join $aof_dir appendonly.aof.manifest] w]
    puts -nonewline $fp "file appendonly.aof.1.incr.aof seq 1 type i\n"
    close $fp
}

tags {"backup external:skip"} {

start_server {overrides {appendonly no auto-aof-rewrite-percentage 0}} {
    set bdirname mybackup
    set backup_server_dir [lindex [r config get dir] 1]
    set bdir [file join $backup_server_dir $bdirname]
    r config set backupdirname $bdirname

    test {BACKUP STATUS reports idle on a fresh instance} {
        assert_equal "idle" [backup_status_field state]
        assert_equal "86400" [lindex [r config get backup-sealed-ttl] 1]
    }

    test {BACKUP read/no-op commands can execute inside transactions} {
        r multi
        assert_equal "QUEUED" [r backup status]
        assert_equal "QUEUED" [r backup list]
        assert_equal "QUEUED" [r backup cleanup]
        set res [r exec]
        array set s [lindex $res 0]
        assert_equal "idle" $s(state)
        assert_equal 0 [llength [lindex $res 1]]
        assert_equal "OK" [lindex $res 2]
    }

    test {BACKUP lifecycle commands can execute inside transactions} {
        r flushall
        r set transaction-backup 1

        r multi
        assert_equal "QUEUED" [r backup start]
        assert_equal {OK} [r exec]
        wait_for_condition 50 100 {
            [backup_status_field state] eq "incrementing"
        } else {
            fail "BACKUP START did not execute from transaction"
        }

        r set transaction-backup-after-start 1
        r multi
        assert_equal "QUEUED" [r backup seal]
        assert_equal {OK} [r exec]
        assert_equal "sealed" [backup_status_field state]

        r multi
        assert_equal "QUEUED" [r backup cleanup]
        assert_equal {OK} [r exec]
        assert_equal "idle" [backup_status_field state]

        r set transaction-backup-abort 1
        r multi
        assert_equal "QUEUED" [r backup start]
        assert_equal {OK} [r exec]
        wait_for_condition 50 100 {
            [backup_status_field state] eq "incrementing"
        } else {
            fail "BACKUP START did not execute from transaction before ABORT"
        }

        r multi
        assert_equal "QUEUED" [r backup abort]
        assert_equal {OK} [r exec]
        assert_equal "failed" [backup_status_field state]
        assert_equal "aborted by user" [backup_status_field error]
        assert_equal "OK" [r backup cleanup]
    }

    test {BACKUP full lifecycle (appendonly no): START -> SEAL -> CLEANUP} {
        r flushall
        r set k1 v1
        r set k2 v2

        assert_equal "OK" [r backup start]
        wait_for_condition 50 100 {
            [backup_status_field state] eq "incrementing"
        } else {
            fail "BACKUP did not reach the incrementing state"
        }

        # Writes during the window must be captured by the pinned INCR.
        r set k3 v3

        # Only the BASE is pinned until SEAL pins the INCR too.
        set files [r backup list]
        assert_equal 1 [llength $files]
        assert_equal "absolute" [file pathtype [lindex $files 0]]
        assert {[file exists [lindex $files 0]]}

        assert_equal "OK" [r backup seal]
        assert_equal "sealed" [backup_status_field state]
        set files [r backup list]
        assert_equal 3 [llength $files]

        # BASE + INCR + manifest must exist in the backup directory.
        assert {[file exists [file join $bdir appendonly.aof.manifest]]}
        foreach f $files {
            assert_equal "absolute" [file pathtype $f]
            assert {[file exists $f]}
        }
    }

    test {Preload a sealed backup via preload-file manifest} {
        # Use the sealed backup produced by the previous test.
        set manifest [file join $bdir appendonly.aof.manifest]
        start_server [list overrides [list appendonly no preload-file "aof:$manifest"]] {
            assert_equal 3 [r dbsize]
            assert_equal v1 [r get k1]
            assert_equal v2 [r get k2]
            assert_equal v3 [r get k3]
            set preload_server_dir [lindex [r config get dir] 1]
            assert {![file exists [file join $preload_server_dir appendonlydir appendonly.aof.manifest]]}
        }
        assert {[file exists $manifest]}
    }

    test {Preload a sealed backup via preload-file manifest with appendonly enabled} {
        set manifest [file join $bdir appendonly.aof.manifest]
        start_server [list overrides [list appendonly yes preload-file "aof:$manifest"]] {
            assert_equal 3 [r dbsize]
            assert_equal v1 [r get k1]
            assert_equal v2 [r get k2]
            assert_equal v3 [r get k3]
            set preload_server_dir [lindex [r config get dir] 1]
            assert {[file exists [file join $preload_server_dir appendonlydir appendonly.aof.manifest]]}
        }
        assert {[file exists $manifest]}
    }

    test {Preload a sealed backup via preload-file manifest skips local RDB} {
        set manifest [file join $bdir appendonly.aof.manifest]
        set local_dir [tmpdir preload.aof.local-rdb]
        r flushall
        r set local-rdb-key should-not-load
        backup_save_rdb_as [file join $local_dir dump.rdb]

        start_server [list overrides [list dir $local_dir appendonly no preload-file "aof:$manifest"]] {
            assert_equal 3 [r dbsize]
            assert_equal v1 [r get k1]
            assert_equal v2 [r get k2]
            assert_equal v3 [r get k3]
            assert_equal 0 [r exists local-rdb-key]
        }
        assert {[file exists $manifest]}
    }

    test {BACKUP CLEANUP removes the sealed backup and returns to idle} {
        assert_equal "OK" [r backup cleanup]
        assert_equal "idle" [backup_status_field state]
        assert_equal "" [backup_status_field error]
        assert {[backup_dir_empty $bdir]}
    }

    test {BACKUP auto-cleans sealed backup after configured ttl} {
        assert_equal "OK" [r config set backup-sealed-ttl 1]
        r flushall
        r set auto-cleanup-backup 1

        assert_equal "OK" [r backup start]
        wait_for_condition 50 100 {
            [backup_status_field state] eq "incrementing"
        } else {
            fail "BACKUP did not reach the incrementing state"
        }
        assert_equal "OK" [r backup seal]
        assert_equal "sealed" [backup_status_field state]

        wait_for_condition 50 100 {
            [backup_status_field state] eq "idle" && [backup_dir_empty $bdir]
        } else {
            fail "Sealed backup was not auto-cleaned"
        }
        assert_equal "OK" [r config set backup-sealed-ttl 86400]
    }

    test {Preload an RDB file via preload-file} {
        r flushall
        r set preload-rdb-key value
        r sadd preload-rdb-set one two
        assert_equal "OK" [r save]

        set preload_rdb [file join $backup_server_dir dump.rdb]
        start_server [list overrides [list appendonly no preload-file "rdb:$preload_rdb"]] {
            assert_equal 2 [r dbsize]
            assert_equal value [r get preload-rdb-key]
            assert_equal {one two} [lsort [r smembers preload-rdb-set]]
            set preload_server_dir [lindex [r config get dir] 1]
            assert {![file exists [file join $preload_server_dir appendonlydir appendonly.aof.manifest]]}
        }
    }

    test {Preload an RDB file via preload-file with appendonly enabled} {
        set preload_rdb [file join $backup_server_dir dump.rdb]
        start_server [list overrides [list appendonly yes preload-file "rdb:$preload_rdb"]] {
            assert_equal 2 [r dbsize]
            assert_equal value [r get preload-rdb-key]
            assert_equal {one two} [lsort [r smembers preload-rdb-set]]
            set preload_server_dir [lindex [r config get dir] 1]
            assert {[file exists [file join $preload_server_dir appendonlydir appendonly.aof.manifest]]}
        }
    }

    test {Preload an RDB file via preload-file skips local RDB} {
        r flushall
        r set preload-rdb-key value
        r sadd preload-rdb-set one two
        set preload_rdb [file join $backup_server_dir preload-source.rdb]
        backup_save_rdb_as $preload_rdb

        set local_dir [tmpdir preload.rdb.local-rdb]
        r flushall
        r set local-rdb-key should-not-load
        backup_save_rdb_as [file join $local_dir dump.rdb]

        start_server [list overrides [list dir $local_dir appendonly no preload-file "rdb:$preload_rdb"]] {
            assert_equal 2 [r dbsize]
            assert_equal value [r get preload-rdb-key]
            assert_equal {one two} [lsort [r smembers preload-rdb-set]]
            assert_equal 0 [r exists local-rdb-key]
        }
    }

    test {Preload an RDB file via preload-file skips local AOF} {
        r flushall
        r set preload-rdb-key value
        r sadd preload-rdb-set one two
        set preload_rdb [file join $backup_server_dir preload-source.rdb]
        backup_save_rdb_as $preload_rdb

        set local_dir [tmpdir preload.rdb.local-aof]
        backup_write_local_aof $local_dir

        start_server [list overrides [list dir $local_dir appendonly yes preload-file "rdb:$preload_rdb"]] {
            assert_equal 2 [r dbsize]
            assert_equal value [r get preload-rdb-key]
            assert_equal {one two} [lsort [r smembers preload-rdb-set]]
            assert_equal 0 [r exists local-aof-key]
        }
    }

    test {appendonly-no backup does not remove preexisting appendonlydir files} {
        set aofdir [file join $backup_server_dir appendonlydir]
        file delete -force $aofdir
        file mkdir $aofdir
        set stale [file join $aofdir stale.aof]
        set fp [open $stale w]
        puts $fp stale
        close $fp

        r flushall
        r set preserve-stale-aof 1
        assert_equal "OK" [r backup start]
        wait_for_condition 50 100 {
            [backup_status_field state] eq "incrementing"
        } else {
            fail "BACKUP did not reach the incrementing state"
        }
        assert_equal "OK" [r backup seal]
        assert {[file exists $stale]}
        assert_equal "OK" [r backup cleanup]
        assert {[file exists $stale]}
        file delete -force $aofdir
    }

    test {BACKUP ABORT cancels an in-progress backup} {
        r backup start
        wait_for_condition 50 100 {
            [backup_status_field state] eq "incrementing"
        } else {
            fail "BACKUP did not reach the incrementing state"
        }
        assert_equal "OK" [r backup abort]
        assert_equal "failed" [backup_status_field state]
        assert_equal "aborted by user" [backup_status_field error]
        assert {[backup_dir_empty $bdir]}
    }

    test {BACKUP errors on invalid state transitions} {
        assert_error "*wrong number*" {r backup}
        assert_error "*No backup in progress*" {r backup abort}
        assert_equal "OK" [r backup cleanup]
        assert_equal "idle" [backup_status_field state]
        assert_error "*No backup ready to seal*" {r backup seal}
    }

    test {BACKUP START rejects a non-empty backup directory} {
        file mkdir $bdir
        set fp [open [file join $bdir stale] w]
        puts $fp stale
        close $fp
        assert_error "*not empty*" {r backup start}
        file delete -force $bdir
    }

    test {CONFIG SET backupdirname rejects paths} {
        assert_error "*can't be a path*" {r config set backupdirname /tmp/mybackup}
        assert_error "*can't be a path*" {r config set backupdirname nested/mybackup}
    }

    test {BACKUP LIST returns absolute paths for a relative backupdirname} {
        set relbdir myrelativebackup
        r config set backupdirname $relbdir
        assert_equal "OK" [r backup start]
        wait_for_condition 50 100 {
            [backup_status_field state] eq "incrementing"
        } else {
            fail "BACKUP did not reach the incrementing state"
        }
        set files [r backup list]
        assert_equal "absolute" [file pathtype [lindex $files 0]]
        assert_match "$backup_server_dir/$relbdir/*" [lindex $files 0]
        assert {[file exists [lindex $files 0]]}
        assert_equal "OK" [r backup abort]
        r config set backupdirname $bdirname
    }

    test {BACKUP START enters pending until a rewrite can start} {
        set aofdir [file join $backup_server_dir appendonlydir]
        file delete -force $aofdir
        r flushall
        r set pending-backup 1
        r config set rdb-key-save-delay 1000000
        r bgsave
        wait_for_condition 50 100 {
            [s rdb_bgsave_in_progress] == 1
        } else {
            fail "BGSAVE did not start"
        }

        assert_equal "OK" [r backup start]
        assert_equal "pending" [backup_status_field state]
        assert_equal 0 [llength [r backup list]]

        file mkdir $aofdir
        set pending_stale [file join $aofdir pending-stale.aof]
        set fp [open $pending_stale w]
        puts $fp stale
        close $fp

        r config set rdb-key-save-delay 0
        wait_for_condition 100 100 {
            [backup_status_field state] eq "incrementing"
        } else {
            fail "BACKUP did not leave pending after BGSAVE finished"
        }

        r set pending-backup-after-start 1
        assert_equal "OK" [r backup seal]
        assert_equal "OK" [r backup cleanup]
        assert {[file exists $pending_stale]}
        file delete -force $aofdir
    }

    test {CONFIG SET appendonly yes while BACKUP START is pending} {
        set aofdir [file join $backup_server_dir appendonlydir]
        file delete -force $aofdir
        r flushall
        r set pending-enable-aof 1
        r config set rdb-key-save-delay 1000000
        r bgsave
        wait_for_condition 50 100 {
            [s rdb_bgsave_in_progress] == 1
        } else {
            fail "BGSAVE did not start"
        }

        assert_equal "OK" [r backup start]
        assert_equal "pending" [backup_status_field state]
        assert_equal "OK" [r config set appendonly yes]

        r config set rdb-key-save-delay 0
        wait_for_condition 100 100 {
            [backup_status_field state] eq "incrementing"
        } else {
            fail "BACKUP did not leave pending after appendonly was enabled"
        }

        r set pending-enable-aof-after-start 1
        assert_equal "OK" [r backup seal]
        assert_equal 1 [s aof_enabled]
        assert_equal "OK" [r backup cleanup]
        assert_equal "OK" [r config set appendonly no]
        file delete -force $aofdir
    }

    test {Disabling AOF aborts an in-progress backup} {
        r config set appendonly yes
        waitForBgrewriteaof r
        r backup start
        wait_for_condition 50 100 {
            [backup_status_field state] eq "incrementing"
        } else {
            fail "BACKUP did not reach the incrementing state"
        }
        r config set appendonly no
        assert_equal "failed" [backup_status_field state]
        assert_equal "appendonly is stopped" [backup_status_field error]
        assert {[backup_dir_empty $bdir]}
    }

    test {BGREWRITEAOF is postponed while an appendonly-no backup is in progress} {
        r flushall
        r set p 1
        r backup start
        wait_for_condition 50 100 {
            [backup_status_field state] eq "incrementing"
        } else {
            fail "BACKUP did not reach the incrementing state"
        }
        set rewrites_before [s aof_rewrites]
        assert_match {*scheduled*} [r bgrewriteaof]
        assert_equal 1 [s aof_rewrite_scheduled]
        assert_equal "OK" [r backup seal]
        wait_for_condition 50 100 {
            [s aof_rewrites] > $rewrites_before
        } else {
            fail "Scheduled BGREWRITEAOF did not run after BACKUP SEAL"
        }
        wait_for_condition 50 100 {
            [s aof_rewrite_scheduled] == 0
        } else {
            fail "Scheduled BGREWRITEAOF was not consumed after BACKUP SEAL"
        }
        waitForBgrewriteaof r
        assert_equal "OK" [r backup cleanup]
        assert {[backup_dir_empty $bdir]}
    }

    test {Enabling AOF during an appendonly-no backup keeps AOF after SEAL} {
        r flushall
        r set enabled-during-backup 1
        assert_equal "OK" [r backup start]
        wait_for_condition 50 100 {
            [backup_status_field state] eq "incrementing"
        } else {
            fail "BACKUP did not reach the incrementing state"
        }
        assert_equal "OK" [r config set appendonly yes]
        assert_equal "OK" [r backup seal]
        assert_equal 1 [s aof_enabled]
        assert {[file exists [file join $backup_server_dir appendonlydir appendonly.aof.manifest]]}
        assert_equal "OK" [r backup cleanup]
        assert {[backup_dir_empty $bdir]}
    }
}

start_server {overrides {appendonly yes auto-aof-rewrite-percentage 0}} {
    set bdirname mybackup
    set bdir [file join [lindex [r config get dir] 1] $bdirname]
    r config set backupdirname $bdirname

    test {BACKUP START reuses an active AOF rewrite} {
        r flushall
        r set reuse-aofrw 1
        r config set rdb-key-save-delay 1000000
        r bgrewriteaof
        wait_for_condition 50 100 {
            [s aof_rewrite_in_progress] == 1
        } else {
            fail "AOF rewrite did not start"
        }

        assert_equal "OK" [r backup start]
        wait_for_condition 50 100 {
            [backup_status_field state] eq "snapshotting"
        } else {
            fail "BACKUP did not start reusing the active AOF rewrite"
        }
        r config set rdb-key-save-delay 0
        wait_for_condition 50 100 {
            [backup_status_field state] eq "incrementing"
        } else {
            fail "BACKUP did not reuse the active AOF rewrite"
        }

        r set reuse-aofrw-after-start 1
        assert_equal "OK" [r backup seal]
        assert_equal "OK" [r backup cleanup]
    }

    test {BACKUP START reuses a scheduled AOF rewrite} {
        r flushall
        r set reuse-scheduled-aofrw 1
        r config set rdb-key-save-delay 1000000
        r bgsave
        wait_for_condition 50 100 {
            [s rdb_bgsave_in_progress] == 1
        } else {
            fail "BGSAVE did not start"
        }

        assert_match {*scheduled*} [r bgrewriteaof]
        assert_equal 1 [s aof_rewrite_scheduled]
        assert_equal "OK" [r backup start]
        assert_equal "pending" [backup_status_field state]
        r config set rdb-key-save-delay 0
        wait_for_condition 100 100 {
            [backup_status_field state] eq "incrementing"
        } else {
            fail "BACKUP did not reuse the scheduled AOF rewrite"
        }

        r set reuse-scheduled-aofrw-after-start 1
        assert_equal "OK" [r backup seal]
        assert_equal "OK" [r backup cleanup]
    }

    test {BACKUP lifecycle works when appendonly is already on} {
        r flushall
        r set a 1
        r set b 2

        assert_equal "OK" [r backup start]
        wait_for_condition 50 100 {
            [backup_status_field state] eq "incrementing"
        } else {
            fail "BACKUP did not reach the incrementing state"
        }
        r set c 3
        assert_equal "OK" [r backup seal]
        assert_equal "sealed" [backup_status_field state]

        # AOF keeps working after the seal.
        r set d 4
        assert_equal 4 [r dbsize]

        assert_equal "OK" [r backup cleanup]
        assert_equal "idle" [backup_status_field state]
    }

    test {BGREWRITEAOF is postponed while a backup is in progress} {
        r backup start
        wait_for_condition 50 100 {
            [backup_status_field state] eq "incrementing"
        } else {
            fail "BACKUP did not reach the incrementing state"
        }
        assert_match {*scheduled*} [r bgrewriteaof]
        assert_equal 1 [s aof_rewrite_scheduled]
        assert_equal "OK" [r backup seal]
        wait_for_condition 50 100 {
            [s aof_rewrite_scheduled] == 0
        } else {
            fail "Scheduled BGREWRITEAOF was not consumed after BACKUP SEAL"
        }
        waitForBgrewriteaof r
        assert_equal "OK" [r backup cleanup]
    }
}

}
