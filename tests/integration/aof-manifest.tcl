source tests/support/aofmanifest.tcl
set defaults { appendonly {yes} appendfilename {appendonly} auto-aof-rewrite-percentage {0}}
set server_path [tmpdir server.aof]
set old_version_aof_path "$server_path/appendonly"
set base_1_aof_path "$server_path/appendonly.1.rdb"
set base_2_aof_path "$server_path/appendonly.2.rdb"
set incr_1_aof_path "$server_path/appendonly.1.aof"
set incr_2_aof_path "$server_path/appendonly.2.aof"
set incr_3_aof_path "$server_path/appendonly.3.aof"
set aof_manifest_path "$server_path/appendonly.manifest"

tags {"external:skip"} {
    start_server {tags {"aof manifest"} overrides {aof-use-rdb-preamble {no} appendfilename {appendonly}}} {
        set dir [get_redis_dir]
        set aof_basename [get_aof_basename $::default_aof_basename]
        set master [srv 0 client]
        set master_host [srv 0 host]
        set master_port [srv 0 port]

        test "AOF make sure aof manifest $::aof_manifest_name not in current working directory" {
            assert_equal 0 [file exists $::aof_manifest_name]
        }

        test "AOF enable will create manifest file" {
            r config set appendonly yes ; # Will create manifest and new INCR aof
            r config set auto-aof-rewrite-percentage 0 ; # Disable auto-rewrite.
            waitForBgrewriteaof r

            # Start write load
            set load_handle0 [start_write_load $master_host $master_port 10]

            wait_for_condition 50 100 {
                [r dbsize] > 0
            } else {
                fail "No write load detected."
            }

            # First AOFRW done
            assert_aof_manifest_content $dir {
                {file appendonly.1.rdb seq 1 type b}
                {file appendonly.1.aof seq 1 type i}
            }

            # Check we really have these files
            assert_equal 1 [check_file_exist $dir $::aof_manifest_name]
            assert_equal 1 [check_file_exist $dir "${aof_basename}.1${::base_aof_sufix}"]
            assert_equal 1 [check_file_exist $dir "${aof_basename}.1${::incr_aof_sufix}"]

            r bgrewriteaof
            waitForBgrewriteaof r

            # The second AOFRW done
            assert_aof_manifest_content $dir {
                {file appendonly.2.rdb seq 2 type b}
                {file appendonly.2.aof seq 2 type i}
            }

            assert_equal 1 [check_file_exist $dir $::aof_manifest_name]
            # Wait bio delete history 
            wait_for_condition 1000 500 {
                [check_file_exist $dir "${aof_basename}.1${::base_aof_sufix}"] == 0 &&
                [check_file_exist $dir "${aof_basename}.1${::incr_aof_sufix}"] == 0
            } else {
                fail "Failed to delete history AOF"
            }
            assert_equal 1 [check_file_exist $dir "${aof_basename}.2${::base_aof_sufix}"]
            assert_equal 1 [check_file_exist $dir "${aof_basename}.2${::incr_aof_sufix}"]

            stop_write_load $load_handle0
            wait_load_handlers_disconnected

            set d1 [r debug digest]
            r debug loadaof
            set d2 [r debug digest]
            assert {$d1 eq $d2}
        }

        test "AOF multiple rewrite failures will open multiple INCR AOFs" {
            # Start write load
            r config set aof-child-rewrite-delay 1000000

            set orig_size [r dbsize]
            set load_handle0 [start_write_load $master_host $master_port 10]

            wait_for_condition 50 100 {
                [r dbsize] > $orig_size
            } else {
                fail "No write load detected."
            }

            # Let AOFRW fail three times
            r bgrewriteaof
            set fork_child_pid [get_child_pid 0]
            exec kill -9 $fork_child_pid
            waitForBgrewriteaof r

            r bgrewriteaof
            set fork_child_pid [get_child_pid 0]
            exec kill -9 $fork_child_pid
            waitForBgrewriteaof r

            r bgrewriteaof
            set fork_child_pid [get_child_pid 0]
            exec kill -9 $fork_child_pid
            waitForBgrewriteaof r

            # We will have four INCR AOFs
            assert_aof_manifest_content $dir {
                {file appendonly.2.rdb seq 2 type b}
                {file appendonly.2.aof seq 2 type i}
                {file appendonly.3.aof seq 3 type i}
                {file appendonly.4.aof seq 4 type i}
                {file appendonly.5.aof seq 5 type i}
            }

            assert_equal 1 [check_file_exist $dir "${aof_basename}.2${::base_aof_sufix}"]
            assert_equal 1 [check_file_exist $dir "${aof_basename}.2${::incr_aof_sufix}"]
            assert_equal 1 [check_file_exist $dir "${aof_basename}.3${::incr_aof_sufix}"]
            assert_equal 1 [check_file_exist $dir "${aof_basename}.4${::incr_aof_sufix}"]
            assert_equal 1 [check_file_exist $dir "${aof_basename}.5${::incr_aof_sufix}"]

            stop_write_load $load_handle0
            wait_load_handlers_disconnected

            set d1 [r debug digest]
            r debug loadaof
            set d2 [r debug digest]
            assert {$d1 eq $d2}

            r config set aof-child-rewrite-delay 0

            # AOFRW success
            r bgrewriteaof
            waitForBgrewriteaof r

            # All previous INCR AOFs have become history
            # and have be deleted
            assert_aof_manifest_content $dir {
                {file appendonly.3.rdb seq 3 type b}
                {file appendonly.6.aof seq 6 type i}
            }

            # Wait bio delete history 
            wait_for_condition 1000 500 {
                [check_file_exist $dir "${aof_basename}.2${::base_aof_sufix}"] == 0 &&
                [check_file_exist $dir "${aof_basename}.2${::incr_aof_sufix}"] == 0 &&
                [check_file_exist $dir "${aof_basename}.3${::incr_aof_sufix}"] == 0 &&
                [check_file_exist $dir "${aof_basename}.4${::incr_aof_sufix}"] == 0 &&
                [check_file_exist $dir "${aof_basename}.5${::incr_aof_sufix}"] == 0
            } else {
                fail "Failed to delete history AOF"
            }

            assert_equal 1 [check_file_exist $dir "${aof_basename}.3${::base_aof_sufix}"]
            assert_equal 1 [check_file_exist $dir "${aof_basename}.6${::incr_aof_sufix}"]

            set d1 [r debug digest]
            r debug loadaof
            set d2 [r debug digest]
            assert {$d1 eq $d2}
        }

        test "AOF rewrite not open new aof when AOF turn off" {
            r config set appendonly no

            r bgrewriteaof
            waitForBgrewriteaof r

            # We only have BASE AOF, no INCR AOF
            assert_aof_manifest_content $dir {
                {file appendonly.4.rdb seq 4 type b}
            }

            assert_equal 1 [check_file_exist $dir "${aof_basename}.4${::base_aof_sufix}"]
            wait_for_condition 1000 500 {
                [check_file_exist $dir "${aof_basename}.6${::incr_aof_sufix}"] == 0 &&
                [check_file_exist $dir "${aof_basename}.7${::incr_aof_sufix}"] == 0
            } else {
                fail "Failed to delete history AOF"
            }

            set d1 [r debug digest]
            r debug loadaof
            set d2 [r debug digest]
            assert {$d1 eq $d2}

            # Turn on AOF again
            r config set appendonly yes
            waitForBgrewriteaof r
           
            # A new INCR AOF was created
            assert_aof_manifest_content $dir {
                {file appendonly.5.rdb seq 5 type b}
                {file appendonly.1.aof seq 1 type i}
            }

            # Wait bio delete history 
            wait_for_condition 1000 500 {
                [check_file_exist $dir "${aof_basename}.4${::base_aof_sufix}"] == 0
            } else {
                fail "Failed to delete history AOF"
            }
            
            assert_equal 1 [check_file_exist $dir "${aof_basename}.5${::base_aof_sufix}"]
            assert_equal 1 [check_file_exist $dir "${aof_basename}.1${::incr_aof_sufix}"]
        }

        test "AOF enable/disable aof auto gc" {
            r config set aof-disable-auto-gc yes

            r bgrewriteaof
            waitForBgrewriteaof r

            r bgrewriteaof
            waitForBgrewriteaof r

            # We can see four history AOFs (Evolved from two BASE and two INCR)
            assert_aof_manifest_content $dir {
                {file appendonly.7.rdb seq 7 type b} 
                {file appendonly.2.aof seq 2 type h} 
                {file appendonly.6.rdb seq 6 type h} 
                {file appendonly.1.aof seq 1 type h} 
                {file appendonly.5.rdb seq 5 type h} 
                {file appendonly.3.aof seq 3 type i}
            }

            assert_equal 1 [check_file_exist $dir "${aof_basename}.5${::base_aof_sufix}"]
            assert_equal 1 [check_file_exist $dir "${aof_basename}.6${::base_aof_sufix}"]
            assert_equal 1 [check_file_exist $dir "${aof_basename}.1${::incr_aof_sufix}"]
            assert_equal 1 [check_file_exist $dir "${aof_basename}.2${::incr_aof_sufix}"]

            r config set aof-disable-auto-gc no

            # Auto gc success
            assert_aof_manifest_content $dir {
                {file appendonly.7.rdb seq 7 type b} 
                {file appendonly.3.aof seq 3 type i}
            }

            # wait bio delete history 
            wait_for_condition 1000 500 {
                [check_file_exist $dir "${aof_basename}.5${::base_aof_sufix}"] == 0 &&
                [check_file_exist $dir "${aof_basename}.6${::base_aof_sufix}"] == 0 &&
                [check_file_exist $dir "${aof_basename}.1${::incr_aof_sufix}"] == 0 &&
                [check_file_exist $dir "${aof_basename}.2${::incr_aof_sufix}"] == 0
            } else {
                fail "Failed to delete history AOF"
            }
        }

        test "AOF reload can recovery the sequence" {
            # Current manifest, BASE seq 7 and INCR seq 3
            assert_aof_manifest_content $dir {
                {file appendonly.7.rdb seq 7 type b} 
                {file appendonly.3.aof seq 3 type i}
            }

            r debug loadaof

            # Trigger AOFRW
            r bgrewriteaof
            waitForBgrewriteaof r

            # Now BASE seq is 8 and INCR seq is 4
            assert_aof_manifest_content $dir {
                {file appendonly.8.rdb seq 8 type b} 
                {file appendonly.4.aof seq 4 type i}
            }
        }

        test "AOF turn on during BGSAVE will not write aof util AOFRW finish" {
            r flushall
            r config set rdb-key-save-delay 10000000
            r config set appendonly no

            r set k1 v1
        
            r bgsave
            wait_for_condition 10 100 {
                [s rdb_bgsave_in_progress] eq 1
            } else {
                fail "bgsave did not start in time"
            }

            # Make server.aof_rewrite_scheduled = 1
            r config set appendonly yes

            # Not open new INCR aof
            assert_aof_manifest_content $dir {
                {file appendonly.8.rdb seq 8 type b} 
                {file appendonly.4.aof seq 4 type i}
            }

            r set k2 v2
            r debug loadaof

            # Both k1 and k2 lost
            assert_equal 0 [r exists k1]
            assert_equal 0 [r exists k2]

            r config set rdb-key-save-delay 0
            waitForBgsave r

            after 2000 ; # Make sure AOFRW was scheduled

            waitForBgrewriteaof r

            assert_aof_manifest_content $dir {
                {file appendonly.9.rdb seq 9 type b} 
                {file appendonly.5.aof seq 5 type i}
            }

            r set k3 v3
            r debug loadaof
            assert_equal v3 [r get k3]
        }

        test "AOF will trigger limit when AOFRW fails many times" {
            r config set aof-child-rewrite-delay 1000000
            # Let us trigger AOFRW easily
            r config set auto-aof-rewrite-percentage 1
            r config set auto-aof-rewrite-min-size 1mb

            # Let AOFRW fail two times, this will trigger AOFRW limit
            r bgrewriteaof
            set fork_child_pid [get_child_pid 0]
            exec kill -9 $fork_child_pid
            waitForBgrewriteaof r

            r bgrewriteaof
            set fork_child_pid [get_child_pid 0]
            exec kill -9 $fork_child_pid
            waitForBgrewriteaof r

            assert_aof_manifest_content $dir {
                {file appendonly.9.rdb seq 9 type b} 
                {file appendonly.5.aof seq 5 type i}
                {file appendonly.6.aof seq 6 type i}
                {file appendonly.7.aof seq 7 type i}
            }

            set orig_size [r dbsize]
            set load_handle0 [start_write_load $master_host $master_port 10]

            wait_for_condition 50 100 {
                [r dbsize] > $orig_size
            } else {
                fail "No write load detected."
            }

            wait_for_condition 1000 500 {
                [count_log_message 0 "triggered the limit"] == 1
            } else {
                fail "aof rewrite did trigger limit"
            }

            # Wait 1 sec
            after 1000

            # No new INCR AOF be created
            assert_aof_manifest_content $dir {
                {file appendonly.9.rdb seq 9 type b} 
                {file appendonly.5.aof seq 5 type i}
                {file appendonly.6.aof seq 6 type i}
                {file appendonly.7.aof seq 7 type i}
            }

            # Turn off auto-rewrite
            r config set auto-aof-rewrite-percentage 0
            r config set aof-child-rewrite-delay 0

            # We can still manually execute AOFRW immediately
            r bgrewriteaof
            waitForBgrewriteaof r

            # Can create New INCR AOF but AOFRW still fail
            assert_equal 1 [check_file_exist $dir "${aof_basename}.8${::incr_aof_sufix}"]

            assert_aof_manifest_content $dir {
                {file appendonly.10.rdb seq 10 type b} 
                {file appendonly.8.aof seq 8 type i}
            }

            stop_write_load $load_handle0
            wait_load_handlers_disconnected

            set d1 [r debug digest]
            r debug loadaof
            set d2 [r debug digest]
            assert {$d1 eq $d2}
        }
    }

    # Tests1: AOF can load data discontinuously increasing sequence
    create_aof $base_1_aof_path {
        append_to_aof [formatCommand set k1 v1]
    }

    create_aof $incr_1_aof_path {
        append_to_aof [formatCommand set k2 v2]
    }

    create_aof $incr_3_aof_path {
        append_to_aof [formatCommand set k3 v3]
    }

    create_aof_manifest $aof_manifest_path {
        append_to_manifest "file appendonly.1.rdb seq 1 type b\n"
        append_to_manifest "file appendonly.1.aof seq 1 type i\n"
        append_to_manifest "file appendonly.3.aof seq 3 type i\n"
    }

    start_server_aof [list dir $server_path] {
        test "AOF can load data discontinuously increasing sequence" {
            assert_equal 1 [is_alive $srv]
            set client [redis [dict get $srv host] [dict get $srv port] 0 $::tls]
            wait_done_loading $client

            assert_equal v1 [$client get k1]
            assert_equal v2 [$client get k2]
            assert_equal v3 [$client get k3]
        }
    }

    # Tests2: AOF can't load data when some aof missing
    catch {exec rm -rf $incr_1_aof_path}
    
    start_server_aof [list dir $server_path] {
        test "AOF can't load data when some aof missing" {
            wait_for_condition 100 50 {
                ! [is_alive $srv]
            } else {
                fail "AOF loading didn't fail"
            }
        }
    }

    clean_aof_persistence $server_path

    # Tests3: AOF can't load data when the sequence not increase monotonically
    create_aof $incr_1_aof_path {
        append_to_aof [formatCommand set k1 v1]
    }

    create_aof $incr_3_aof_path {
        append_to_aof [formatCommand set k2 v2]
    }

    create_aof_manifest $aof_manifest_path {
        append_to_manifest "file appendonly.3.aof seq 3 type i\n"
        append_to_manifest "file appendonly.1.aof seq 1 type i\n"
    }

    start_server_aof [list dir $server_path] {
        test "AOF can't load data when the sequence not increase monotonically" {
            wait_for_condition 100 50 {
                ! [is_alive $srv]
            } else {
                fail "AOF loading didn't fail"
            }
        }
    }

    clean_aof_persistence $server_path

    # Tests4: AOF can't load data when there are blank lines in the manifest file
    create_aof $incr_1_aof_path {
        append_to_aof [formatCommand set k1 v1]
    }

    create_aof $incr_3_aof_path {
        append_to_aof [formatCommand set k2 v2]
    }

    create_aof_manifest $aof_manifest_path {
        append_to_manifest "file appendonly.1.aof seq 1 type i\n"
        append_to_manifest "\n"
        append_to_manifest "file appendonly.3.aof seq 3 type i\n"
    }

    start_server_aof [list dir $server_path] {
        test "AOF can't load data when there are blank lines in the manifest file" {
            wait_for_condition 100 50 {
                ! [is_alive $srv]
            } else {
                fail "AOF loading didn't fail"
            }
        }
    }

    catch {exec rm -rf $incr_1_aof_path}
    catch {exec rm -rf $incr_3_aof_path}
    catch {exec rm -rf $aof_manifest_path}

    # Tests5: AOF can't load data when there is a duplicate base aof
    create_aof $base_1_aof_path {
        append_to_aof [formatCommand set k1 v1]
    }

    create_aof $base_2_aof_path {
        append_to_aof [formatCommand set k2 v2]
    }

    create_aof $incr_1_aof_path {
        append_to_aof [formatCommand set k3 v3]
    }

    create_aof_manifest $aof_manifest_path {
        append_to_manifest "file appendonly.1.rdb seq 1 type b\n"
        append_to_manifest "file appendonly.2.rdb seq 2 type b\n"
        append_to_manifest "file appendonly.1.aof seq 1 type i\n"
    }

    start_server_aof [list dir $server_path] {
        test "AOF can't load data when there is a duplicate base aof" {
            wait_for_condition 100 50 {
                ! [is_alive $srv]
            } else {
                fail "AOF loading didn't fail"
            }
        }
    }

    clean_aof_persistence $server_path

    # Tests6: AOF can't load data when the manifest format is wrong (type unknown)
    create_aof $base_1_aof_path {
        append_to_aof [formatCommand set k1 v1]
    }

    create_aof $incr_1_aof_path {
        append_to_aof [formatCommand set k3 v3]
    }

    create_aof_manifest $aof_manifest_path {
        append_to_manifest "file appendonly.1.rdb seq 1 type x\n"
        append_to_manifest "file appendonly.1.aof seq 1 type i\n"
    }

    start_server_aof [list dir $server_path] {
        test "AOF can't load data when the manifest format is wrong (type unknown)" {
            wait_for_condition 100 50 {
                ! [is_alive $srv]
            } else {
                fail "AOF loading didn't fail"
            }
        }
    }

    clean_aof_persistence $server_path

    # Tests7: AOF can't load data when the manifest format is wrong (file typo)
    create_aof $base_1_aof_path {
        append_to_aof [formatCommand set k1 v1]
    }

    create_aof $incr_1_aof_path {
        append_to_aof [formatCommand set k3 v3]
    }

    create_aof_manifest $aof_manifest_path {
        append_to_manifest "filenema appendonly.1.rdb seq 1 type b\n"
        append_to_manifest "file appendonly.1.aof seq 1 type i\n"
    }

    start_server_aof [list dir $server_path] {
        test "AOF can't load data when the manifest format is wrong (file typo)" {
            wait_for_condition 100 50 {
                ! [is_alive $srv]
            } else {
                fail "AOF loading didn't fail"
            }
        }
    }

    clean_aof_persistence $server_path

    # Tests8: AOF can load data from old redis aof
    create_aof $old_version_aof_path {
        append_to_aof [formatCommand set k1 v1]
        append_to_aof [formatCommand set k2 v2]
        append_to_aof [formatCommand set k3 v3]
    }

    start_server_aof [list dir $server_path] {
        test "AOF can load data from old redis aof" {
            assert_equal 1 [is_alive $srv]

            set client [redis [dict get $srv host] [dict get $srv port] 0 $::tls]
            wait_done_loading $client

            assert_equal v1 [$client get k1]
            assert_equal v2 [$client get k2]
            assert_equal v3 [$client get k3]

            assert_aof_manifest_content $server_path {
                {file appendonly seq 1 type b} 
                {file appendonly.1.aof seq 1 type i}
            }

            assert_equal OK [$client set k4 v4]
            
            $client bgrewriteaof
            while 1 {
                if {[status $client aof_rewrite_in_progress] eq 1} {
                    if {$::verbose} {
                        puts -nonewline "\nWaiting for background AOF rewrite to finish... "
                        flush stdout
                    }
                    after 1000
                } else {
                    break
                }
            }

            assert_equal OK [$client set k5 v5]

            assert_aof_manifest_content $server_path {
                {file appendonly.2.rdb seq 2 type b} 
                {file appendonly.2.aof seq 2 type i}
            }

            set d1 [$client debug digest]
            $client debug loadaof
            set d2 [$client debug digest]
            assert {$d1 eq $d2}
        }
    }

    clean_aof_persistence $server_path

    # Tests9: AOF can't load data when the manifest file is empty
    create_aof_manifest $aof_manifest_path {
    }

    start_server_aof [list dir $server_path] {
        test "AOF can't load data when the manifest file is empty" {
            wait_for_condition 100 50 {
                ! [is_alive $srv]
            } else {
                fail "AOF loading didn't fail"
            }
        }
    }

    clean_aof_persistence $server_path

    # Tests10: AOF can start when no aof and no manifest
    start_server_aof [list dir $server_path] {
        test "AOF can start when no aof and no manifest" {
            assert_equal 1 [is_alive $srv]

            set client [redis [dict get $srv host] [dict get $srv port] 0 $::tls]

            assert_equal OK [$client set k1 v1]
            assert_equal v1 [$client get k1]
        }
    }
}
