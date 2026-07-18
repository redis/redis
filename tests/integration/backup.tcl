proc backup_status_field {field} {
    array set s [r backup status]
    return $s($field)
}

proc dir_empty {dir} {
    return [expr {[file exists $dir] && [llength [glob -nocomplain -directory $dir *]] == 0}]
}

# Create a local RDB file from the current dataset.
proc create_local_rdb {path} {
    assert_equal "OK" [r save]
    set dir [lindex [r config get dir] 1]
    file copy -force [file join $dir dump.rdb] $path
}

# Create a local MP-AOF directory. The marker key lets preload tests prove
# that this local AOF was skipped.
proc create_local_aof {dir} {
    set aof_dir [file join $dir appendonlydir]
    file mkdir $aof_dir
    set fp [open [file join $aof_dir appendonly.aof.1.incr.aof] w]
    # the client uses db 9 by default
    puts -nonewline $fp [formatCommand select 9]
    puts -nonewline $fp [formatCommand set local-aof-key value]
    close $fp

    set fp [open [file join $aof_dir appendonly.aof.manifest] w]
    puts -nonewline $fp "file appendonly.aof.1.incr.aof seq 1 type i\n"
    close $fp
}

tags {"backup external:skip"} {

start_server {overrides {appendonly no auto-aof-rewrite-percentage 0}} {
    set bdirname backupdir
    set server_dir [lindex [r config get dir] 1]
    set bdir [file join $server_dir $bdirname]

    test {BACKUP HELP outputs subcommand help} {
        assert_match "*BACKUP <subcommand> *" [r BACKUP HELP]
    }

    test {BACKUP STATUS reports idle on a fresh instance} {
        array set s [r backup status]
        assert_equal "idle" $s(state)
        assert_equal "" $s(error)
        assert_equal "0" $s(start_time)
        assert_equal "0" $s(end_time)
        assert_equal 0 [s backup_in_progress]
        assert_equal "0" [lindex [r config get backup-sealed-ttl] 1]
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
        assert_equal 1 [s backup_in_progress]

        # Writes during the window must be captured by the pinned INCR.
        r set k3 v3

        # Only the BASE is pinned until SEAL pins the INCR too.
        set files [r backup list]
        assert_equal 1 [llength $files]
        assert_match "appendonly.aof.*.base.rdb" [file tail [lindex $files 0]]
        assert {[file exists [lindex $files 0]]}

        assert_equal "OK" [r backup seal]
        assert_equal 0 [s backup_in_progress]
        array set s [r backup status]
        assert_equal "sealed" $s(state)
        assert_equal "" $s(error)
        assert {$s(start_time) > 0}
        assert {$s(end_time) >= $s(start_time)}

        # SEAL freezes the complete artifact set.
        set files [r backup list]
        assert_equal 3 [llength $files]
        assert_match "appendonly.aof.*.base.rdb" [file tail [lindex $files 0]]
        assert_match "appendonly.aof.*.incr.aof" [file tail [lindex $files 1]]
        assert_equal "appendonly.aof.manifest" [file tail [lindex $files 2]]

        # BASE + INCR + manifest must exist in the backup directory.
        foreach f $files {
            assert {[file exists $f]}
        }

        # The pinned INCR AOF does not grow after SEAL.
        set incr [lindex $files 1]
        set incr_size [file size $incr]
        r set k4 v4
        assert_equal 4 [r dbsize]
        assert_equal $incr_size [file size $incr]
    }

    test {Preload a sealed backup via preload-file manifest} {
        # Use the sealed backup produced by the previous test.
        set manifest [file join $bdir appendonly.aof.manifest]
        foreach enable {no yes} {
            start_server [list overrides [list appendonly $enable preload-file "aof:$manifest"]] {
                assert_equal 3 [r dbsize]
                assert_equal v1 [r get k1]
                assert_equal v2 [r get k2]
                assert_equal v3 [r get k3]
                # appendonly yes creates a new local AOF manifest after preload.
                if {$enable eq "yes"} {
                    set preload_server_dir [lindex [r config get dir] 1]
                    assert {[file exists [file join $preload_server_dir appendonlydir appendonly.aof.manifest]]}
                }
            }
        }
    }

    test {Preload proactively rewrites the local AOF} {
        set local_dir [tmpdir preload.rewrite.local-aof]
        # Seed an unrelated local MP-AOF. preload-file must replace this
        # manifest history rather than append the restored data to it.
        create_local_aof $local_dir
        set aof_dir [file join $local_dir appendonlydir]
        set old_incr [file join $aof_dir appendonly.aof.1.incr.aof]

        set manifest [file join $bdir appendonly.aof.manifest]
        start_server [list overrides [list dir $local_dir appendonly yes preload-file "aof:$manifest"] keep_persistence true] {
            assert_equal 3 [r dbsize]
            assert_equal 1 [s aof_rewrites]
            waitForBgrewriteaof r
            assert_equal ok [s aof_last_bgrewrite_status]
            # we don't append the old incr aof, and it is deleted after AOFRW
            assert_equal 0 [file exists $old_incr]
            assert_equal OK [r set after-preload value]
        }

        # Restart without preload-file to prove the rewrite replaced the old
        # local AOF and persisted both the preloaded data and subsequent writes.
        start_server [list overrides [list dir $local_dir appendonly yes]] {
            assert_equal 4 [r dbsize]
            assert_equal v1 [r get k1]
            assert_equal v2 [r get k2]
            assert_equal v3 [r get k3]
            assert_equal value [r get after-preload]
        }
    }

    test {Preload current local manifest does not rewrite AOF} {
        # Build a local MP-AOF and use its exact absolute manifest path as the
        # preload source, matching the manifest Redis would normally load.
        set local_dir [file normalize [tmpdir preload.current-manifest]]
        set manifest [file join $local_dir appendonlydir appendonly.aof.manifest]
        create_local_aof $local_dir

        start_server [list overrides [list dir $local_dir appendonly yes preload-file "aof:$manifest"]] {
            assert_equal value [r get local-aof-key]
            # Reusing the current manifest must not trigger a redundant AOFRW.
            assert_equal 0 [s aof_rewrites]
        }
    }

    test {Preload a sealed backup via preload-file manifest skips local RDB} {
        set manifest [file join $bdir appendonly.aof.manifest]
        set local_dir [tmpdir preload.aof.local-rdb]

        # Put a local RDB in the server dir; preload-file must override it.
        r flushall
        r set local-rdb-key should-not-load
        create_local_rdb [file join $local_dir dump.rdb]

        start_server [list overrides [list dir $local_dir appendonly no preload-file "aof:$manifest"]] {
            # Only the preload manifest should be loaded.
            assert_equal 3 [r dbsize]
            assert_equal v1 [r get k1]
            assert_equal v2 [r get k2]
            assert_equal v3 [r get k3]
            # The local RDB marker key must not appear.
            assert_equal 0 [r exists local-rdb-key]
        }
    }

    test {Preload a sealed backup via preload-file manifest skips local AOF} {
        set manifest [file join $bdir appendonly.aof.manifest]
        set local_dir [tmpdir preload.aof.local-aof]

        # Put a local AOF in the server dir; preload-file must override it.
        create_local_aof $local_dir

        start_server [list overrides [list dir $local_dir appendonly yes preload-file "aof:$manifest"]] {
            # Only the preload manifest should be loaded.
            assert_equal 3 [r dbsize]
            assert_equal v1 [r get k1]
            assert_equal v2 [r get k2]
            assert_equal v3 [r get k3]
            # The local AOF marker key must not appear.
            assert_equal 0 [r exists local-aof-key]
        }
    }

    test {Preload file validates prefix and extension} {
        # Reject ambiguous preload-file values before startup loading dispatches
        # to the RDB or AOF path.
        catch {exec src/redis-server --port 0 --preload-file aof:/tmp/foo} err
        assert_match {*preload-file must end with an extension*} $err
        catch {exec src/redis-server --port 0 --preload-file rdb:/tmp/foo} err
        assert_match {*preload-file must end with an extension*} $err
        catch {exec src/redis-server --port 0 --preload-file invalid:/tmp/foo.rdb} err
        assert_match {*argument must be in the format*} $err
    }

    test {BACKUP CLEANUP removes the sealed backup and returns to idle} {
        # Clean up the sealed backup produced by the lifecycle test above.
        assert_equal "OK" [r backup cleanup]
        assert_equal "idle" [backup_status_field state]
        assert_equal "" [backup_status_field error]
        assert {[dir_empty $bdir]}
    }

    test {BACKUP auto-cleans sealed backup after configured ttl} {
        # Use a short TTL so the test can verify automatic reclamation.
        assert_equal "OK" [r config set backup-sealed-ttl 1]
        r flushall
        r set key val

        assert_equal "OK" [r backup start]
        wait_for_condition 50 100 {
            [backup_status_field state] eq "incrementing"
        } else {
            fail "BACKUP did not reach the incrementing state"
        }
        assert_equal "OK" [r backup seal]
        assert_equal "sealed" [backup_status_field state]

        wait_for_condition 50 100 {
            [backup_status_field state] eq "idle" && [dir_empty $bdir]
        } else {
            fail "Sealed backup was not auto-cleaned"
        }
        assert_equal "OK" [r config set backup-sealed-ttl 0]
    }

    test {Preload an RDB file via preload-file} {
        r flushall
        r set preload-rdb-key value
        r sadd preload-rdb-set one two
        assert_equal "OK" [r save]

        # The rdb: preload path should work regardless of the runtime AOF mode.
        set preload_rdb [file join $server_dir dump.rdb]
        foreach enable {no yes} {
            start_server [list overrides [list appendonly $enable preload-file "rdb:$preload_rdb"]] {
                assert_equal 2 [r dbsize]
                assert_equal value [r get preload-rdb-key]
                assert_equal {one two} [lsort [r smembers preload-rdb-set]]
            }
        }
    }

    test {Preload an RDB file via preload-file skips local RDB} {
        r flushall
        r set preload-rdb-key value
        r sadd preload-rdb-set one two
        set preload_rdb [file join $server_dir preload-source.rdb]
        create_local_rdb $preload_rdb

        # A second local RDB would be loaded by normal startup, but preload-file
        # must take precedence.
        set local_dir [tmpdir preload.rdb.local-rdb]
        r flushall
        r set local-rdb-key should-not-load
        create_local_rdb [file join $local_dir dump.rdb]

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
        set preload_rdb [file join $server_dir preload-source.rdb]
        create_local_rdb $preload_rdb

        # A local AOF would be loaded by normal appendonly startup, but
        # preload-file must take precedence.
        set local_dir [tmpdir preload.rdb.local-aof]
        create_local_aof $local_dir

        start_server [list overrides [list dir $local_dir appendonly yes preload-file "rdb:$preload_rdb"]] {
            assert_equal 2 [r dbsize]
            assert_equal value [r get preload-rdb-key]
            assert_equal {one two} [lsort [r smembers preload-rdb-set]]
            assert_equal 0 [r exists local-aof-key]
        }
    }

    test {appendonly-no backup does not remove generated appendonlydir files when the directory is preexisting} {
        # Make appendonlydir non-empty before BACKUP starts so Redis cannot
        # assume it owns the whole directory.
        set aofdir [file join $server_dir appendonlydir]
        file delete -force $aofdir
        file mkdir $aofdir
        # This stale file only marks the directory as preexisting.
        set stale [file join $aofdir stale.aof]
        set fp [open $stale w]
        puts $fp stale
        close $fp

        r flushall
        r set key val
        assert_equal "OK" [r backup start]
        wait_for_condition 50 100 {
            [backup_status_field state] eq "incrementing"
        } else {
            fail "BACKUP did not reach the incrementing state"
        }
        assert_equal "OK" [r backup seal]

        # These are the live AOF files BACKUP created while appendonly was off.
        # Since appendonlydir was preexisting, CLEANUP must leave them in place.
        set generated_aofs [glob -nocomplain -directory $aofdir appendonly.aof*]
        assert {[llength $generated_aofs] > 0}
        assert_equal "OK" [r backup cleanup]
        foreach f $generated_aofs {
            assert {[file exists $f]}
        }
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
        assert {[dir_empty $bdir]}
    }

    test {BACKUP errors on invalid state transitions} {
        # Exercise the command-family state guardrails from idle/no-backup state.
        assert_error "*wrong number*" {r backup}
        assert_error "*No backup in progress*" {r backup abort}
        assert_equal "OK" [r backup cleanup]
        assert_equal "idle" [backup_status_field state]
        assert_error "*No backup ready to seal*" {r backup seal}
    }

    test {BACKUP START rejects a non-empty backup directory} {
        # Redis should never delete or reuse unknown files in backupdirname.
        file mkdir $bdir
        set fp [open [file join $bdir stale] w]
        puts $fp stale
        close $fp
        assert_error "*not empty*" {r backup start}
        file delete -force $bdir
    }

    test {BACKUP START can execute inside transactions} {
        r flushall

        r multi
        assert_equal "QUEUED" [r set transaction-before-backup-start before]
        assert_equal "QUEUED" [r backup start]
        assert_equal "QUEUED" [r set transaction-after-backup-start after]
        assert_equal {OK OK OK} [r exec]
        wait_for_condition 50 100 {
            [backup_status_field state] eq "incrementing"
        } else {
            fail "BACKUP START did not execute from transaction"
        }

        # The snapshot itself must include the whole transaction, not split at BACKUP START.
        set files [r backup list]
        set base [lindex $files 0]
        start_server [list overrides [list appendonly no preload-file "rdb:$base"]] {
            assert_equal before [r get transaction-before-backup-start]
            assert_equal after [r get transaction-after-backup-start]
        }
        # The transaction snapshot was verified; discard the in-progress backup.
        assert_equal "OK" [r backup abort]
        assert_equal "OK" [r backup cleanup]
    }

    test {backupdirname rejects paths at startup} {
        # backupdirname is a dirname under server dir, not a configurable path.
        catch {exec src/redis-server --backupdirname /tmp/mybackup} err
        assert_match {*backupdirname can't be a path*} $err
        catch {exec src/redis-server --backupdirname nested/mybackup} err
        assert_match {*backupdirname can't be a path*} $err
    }

    test {BACKUP LIST returns absolute paths for a relative backupdirname} {
        # The config stores a relative dirname, but the data plane needs
        # absolute paths returned by BACKUP LIST.
        assert_equal "OK" [r backup start]
        wait_for_condition 50 100 {
            [backup_status_field state] eq "incrementing"
        } else {
            fail "BACKUP did not reach the incrementing state"
        }
        set files [r backup list]
        assert_equal 1 [llength $files]
        assert_equal "absolute" [file pathtype [lindex $files 0]]
        assert_match "$server_dir/$bdirname/*" [lindex $files 0]
        assert {[file exists [lindex $files 0]]}

        assert_equal "OK" [r backup seal]
        set files [r backup list]
        assert_equal 3 [llength $files]
        foreach f $files {
            assert_equal "absolute" [file pathtype $f]
            assert_match "$server_dir/$bdirname/*" $f
            assert {[file exists $f]}
        }
        assert_equal "OK" [r backup cleanup]
    }

    test {BACKUP START enters pending until a rewrite can start} {
        r flushall
        # Keep BGSAVE busy long enough for BACKUP START to observe an active child.
        for {set i 0} {$i < 3} {incr i} {
            r set key:$i $i
        }
        r config set rdb-key-save-delay 2000000
        r bgsave
        wait_for_condition 50 100 {
            [s rdb_bgsave_in_progress] == 1
        } else {
            fail "BGSAVE did not start"
        }

        assert_equal "OK" [r backup start]
        assert_equal "pending" [backup_status_field state]
        assert_equal 0 [llength [r backup list]]

        # Once BGSAVE exits, cron can start the deferred AOFRW and publish BASE.
        r config set rdb-key-save-delay 0
        wait_for_condition 100 100 {
            [backup_status_field state] eq "incrementing"
        } else {
            fail "BACKUP did not leave pending after BGSAVE finished"
        }
        assert_equal "OK" [r backup seal]
        assert_equal "OK" [r backup cleanup]
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
        # Disabling appendonly stops the INCR stream, so the backup must fail.
        r config set appendonly no
        assert_equal "failed" [backup_status_field state]
        assert_equal "appendonly is stopped" [backup_status_field error]
        assert {[dir_empty $bdir]}
    }

    test {BGREWRITEAOF is postponed while an appendonly-no backup is in progress} {
        r flushall
        r set key val
        r backup start
        wait_for_condition 50 100 {
            [backup_status_field state] eq "incrementing"
        } else {
            fail "BACKUP did not reach the incrementing state"
        }
        set rewrites_before [s aof_rewrites]

        # BACKUP owns the temporary AOF stream, so manual BGREWRITEAOF is postponed.
        assert_match {*scheduled*} [r bgrewriteaof]
        assert_equal 1 [s aof_rewrite_scheduled]

        # SEAL closes the backup window and lets the postponed rewrite run.
        assert_equal "OK" [r backup seal]
        wait_for_condition 50 100 {
            [s aof_rewrites] > $rewrites_before
        } else {
            fail "Scheduled BGREWRITEAOF did not run after BACKUP SEAL"
        }
        assert_equal 0 [s aof_rewrite_scheduled]
        waitForBgrewriteaof r
        assert_equal "OK" [r backup cleanup]
        assert {[dir_empty $bdir]}
    }

    test {Enabling AOF during an appendonly-no backup keeps AOF after SEAL} {
        r flushall
        r set key val
        assert_equal "OK" [r backup start]
        wait_for_condition 50 100 {
            [backup_status_field state] eq "incrementing"
        } else {
            fail "BACKUP did not reach the incrementing state"
        }
        # Once appendonly is enabled, the temporary AOF becomes the live AOF.
        assert_equal "OK" [r config set appendonly yes]
        assert_equal "OK" [r backup seal]
        assert_equal 1 [s aof_enabled]
        assert_equal "OK" [r backup cleanup]
        assert {[dir_empty $bdir]}
        # CLEANUP removes backup artifacts, not the live appendonlydir state.
        assert {[file exists [file join $server_dir appendonlydir appendonly.aof.manifest]]}
    }
}

start_server {overrides {appendonly yes auto-aof-rewrite-percentage 0}} {
    set bdirname backupdir
    set bdir [file join [lindex [r config get dir] 1] $bdirname]

    test {BACKUP START reuses an active AOF rewrite} {
        r flushall
        # Keep AOFRW active long enough for BACKUP START to attach to it.
        for {set i 0} {$i < 3} {incr i} {
            r set key:$i $i
        }
        r config set rdb-key-save-delay 2000000
        r bgrewriteaof
        wait_for_condition 50 100 {
            [s aof_rewrite_in_progress] == 1
        } else {
            fail "AOF rewrite did not start"
        }

        assert_equal "OK" [r backup start]
        # BACKUP should attach to the already-running AOFRW instead of starting a new one.
        wait_for_condition 50 100 {
            [backup_status_field state] eq "snapshotting"
        } else {
            fail "BACKUP did not start reusing the active AOF rewrite"
        }
        # Let the reused AOFRW finish and become the backup snapshot.
        r config set rdb-key-save-delay 0
        wait_for_condition 100 100 {
            [backup_status_field state] eq "incrementing"
        } else {
            fail "BACKUP did not reuse the active AOF rewrite"
        }

        assert_equal "OK" [r backup seal]
        assert_equal "OK" [r backup cleanup]
    }

    test {BACKUP START reuses a scheduled AOF rewrite} {
        r flushall
        # Keep BGSAVE busy so BGREWRITEAOF is scheduled instead of started.
        for {set i 0} {$i < 3} {incr i} {
            r set key:$i $i
        }
        r config set rdb-key-save-delay 2000000
        r bgsave
        wait_for_condition 50 100 {
            [s rdb_bgsave_in_progress] == 1
        } else {
            fail "BGSAVE did not start"
        }

        assert_match {*scheduled*} [r bgrewriteaof]
        assert_equal 1 [s aof_rewrite_scheduled]
        # BACKUP should wait for the scheduled AOFRW instead of starting another rewrite.
        assert_equal "OK" [r backup start]
        assert_equal "pending" [backup_status_field state]
        # Let BGSAVE finish so the scheduled AOFRW can run and produce the backup snapshot.
        r config set rdb-key-save-delay 0
        wait_for_condition 100 100 {
            [backup_status_field state] eq "incrementing"
        } else {
            fail "BACKUP did not reuse the scheduled AOF rewrite"
        }

        assert_equal "OK" [r backup seal]
        assert_equal "OK" [r backup cleanup]
    }

    test {BACKUP lifecycle works when appendonly is already on} {
        r flushall
        r set a 1
        r set b 2

        # With appendonly already enabled, BACKUP should reuse MP-AOF without
        # disabling the live AOF stream.
        assert_equal "OK" [r backup start]
        wait_for_condition 50 100 {
            [backup_status_field state] eq "incrementing"
        } else {
            fail "BACKUP did not reach the incrementing state"
        }
        r set c 3
        assert_equal "OK" [r backup seal]
        assert_equal "sealed" [backup_status_field state]

        set files [r backup list]
        assert_equal 3 [llength $files]
        assert_match "appendonly.aof.*.base.rdb" [file tail [lindex $files 0]]
        assert_match "appendonly.aof.*.incr.aof" [file tail [lindex $files 1]]
        assert_equal "appendonly.aof.manifest" [file tail [lindex $files 2]]
        set incr [lindex $files 1]
        set manifest [lindex $files 2]
        set incr_size [file size $incr]

        # AOF keeps working after the seal.
        r set d 4
        assert_equal 4 [r dbsize]
        assert_equal $incr_size [file size $incr]

        # Restoring from the sealed manifest should include writes through SEAL,
        # but not later writes that went only to the live AOF.
        start_server [list overrides [list appendonly no preload-file "aof:$manifest"]] {
            assert_equal 3 [r dbsize]
            assert_equal 1 [r get a]
            assert_equal 2 [r get b]
            assert_equal 3 [r get c]
            assert_equal 0 [r exists d]
        }

        assert_equal "OK" [r backup cleanup]
        assert_equal "idle" [backup_status_field state]
    }

}

}
