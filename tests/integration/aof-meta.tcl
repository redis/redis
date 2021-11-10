source tests/support/aofmeta.tcl
set defaults { appendonly {yes} appendfilename {appendonly.aof} auto-aof-rewrite-percentage {0}}
set server_path [tmpdir server.aof]
set base_1_aof_path "$server_path/appendonly.aof_b_1"
set base_2_aof_path "$server_path/appendonly.aof_b_2"
set incr_1_aof_path "$server_path/appendonly.aof_i_1"
set incr_3_aof_path "$server_path/appendonly.aof_i_3"
set aof_meta_path "$server_path/appendonly.aof_manifest"

# delete the meta file before start_server
exec rm -rf $::aof_meta_filename
tags {"external:skip"} {
    start_server {tags {"aof meta"} overrides {aof-use-rdb-preamble {no} appendfilename {appendonly.aof}}} {
        set dir [get_redis_dir]
        set aof_basename [get_aof_basename $::default_aof_basename]
        set master [srv 0 client]
        set master_host [srv 0 host]
        set master_port [srv 0 port]

        test "AOF make sure $::aof_meta_filename not in current working directory" {
            assert_equal 0 [file exists $::aof_meta_filename]
        }

        test "AOF enbale will create meta file" {
            r config set appendonly yes
            r config set auto-aof-rewrite-percentage 0 ; # Disable auto-rewrite.
            waitForBgrewriteaof r

            set load_handle0 [start_write_load $master_host $master_port 10]

            wait_for_condition 50 100 {
                [r dbsize] > 0
            } else {
                fail "No write load detected."
            }

            assert_aof_meta_content $dir {
                {fileName appendonly.aof_b_1 fileSeq 1 fileType b}
                {fileName appendonly.aof_i_1 fileSeq 1 fileType i}
            }

            assert_equal 1 [check_file_exist $dir $::aof_meta_filename]
            assert_equal 1 [check_file_exist $dir "${aof_basename}${::base_aof_sufix}1"]
            assert_equal 1 [check_file_exist $dir "${aof_basename}${::incr_aof_sufix}1"]

            r bgrewriteaof
            waitForBgrewriteaof r

            after 3000 ; # Wait aof gc cron 

            assert_aof_meta_content $dir {
                {fileName appendonly.aof_b_2 fileSeq 2 fileType b}
                {fileName appendonly.aof_i_2 fileSeq 2 fileType i}
            }

            assert_equal 1 [check_file_exist $dir $::aof_meta_filename]
            assert_equal 0 [check_file_exist $dir "${aof_basename}${::base_aof_sufix}1"]
            assert_equal 0 [check_file_exist $dir "${aof_basename}${::incr_aof_sufix}1"]
            assert_equal 1 [check_file_exist $dir "${aof_basename}${::base_aof_sufix}2"]
            assert_equal 1 [check_file_exist $dir "${aof_basename}${::incr_aof_sufix}2"]

            stop_write_load $load_handle0
            wait_load_handlers_disconnected

            set d1 [r debug digest]
            r debug loadaof
            set d2 [r debug digest]
            assert {$d1 eq $d2}
        }

        test "AOF multiple rewrite failures will open multiple INCR AOFs" {
            r config set aof-child-rewrite-delay 1000000

            set orig_size [r dbsize]
            set load_handle0 [start_write_load $master_host $master_port 10]

            wait_for_condition 50 100 {
                [r dbsize] > $orig_size
            } else {
                fail "No write load detected."
            }

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

            assert_aof_meta_content $dir {
                {fileName appendonly.aof_b_2 fileSeq 2 fileType b}
                {fileName appendonly.aof_i_2 fileSeq 2 fileType i}
                {fileName appendonly.aof_i_3 fileSeq 3 fileType i}
                {fileName appendonly.aof_i_4 fileSeq 4 fileType i}
                {fileName appendonly.aof_i_5 fileSeq 5 fileType i}
            }

            assert_equal 1 [check_file_exist $dir "${aof_basename}${::base_aof_sufix}2"]
            assert_equal 1 [check_file_exist $dir "${aof_basename}${::incr_aof_sufix}2"]
            assert_equal 1 [check_file_exist $dir "${aof_basename}${::incr_aof_sufix}3"]
            assert_equal 1 [check_file_exist $dir "${aof_basename}${::incr_aof_sufix}4"]
            assert_equal 1 [check_file_exist $dir "${aof_basename}${::incr_aof_sufix}5"]

            stop_write_load $load_handle0
            wait_load_handlers_disconnected

            set d1 [r debug digest]
            r debug loadaof
            set d2 [r debug digest]
            assert {$d1 eq $d2}

            r config set aof-child-rewrite-delay 0

            r bgrewriteaof
            waitForBgrewriteaof r

            after 3000 ; # Wait aof gc cron 

            assert_aof_meta_content $dir {
                {fileName appendonly.aof_b_3 fileSeq 3 fileType b}
                {fileName appendonly.aof_i_6 fileSeq 6 fileType i}
            }

            assert_equal 0 [check_file_exist $dir "${aof_basename}${::base_aof_sufix}2"]
            assert_equal 0 [check_file_exist $dir "${aof_basename}${::incr_aof_sufix}2"]
            assert_equal 0 [check_file_exist $dir "${aof_basename}${::incr_aof_sufix}3"]
            assert_equal 0 [check_file_exist $dir "${aof_basename}${::incr_aof_sufix}4"]
            assert_equal 0 [check_file_exist $dir "${aof_basename}${::incr_aof_sufix}5"]

            assert_equal 1 [check_file_exist $dir "${aof_basename}${::base_aof_sufix}3"]
            assert_equal 1 [check_file_exist $dir "${aof_basename}${::incr_aof_sufix}6"]

            set d1 [r debug digest]
            r debug loadaof
            set d2 [r debug digest]
            assert {$d1 eq $d2}
        }

        test "AOF rewrite not open new aof when AOF turn off" {
            r config set appendonly no

            r bgrewriteaof
            waitForBgrewriteaof r

            after 3000 ; # Wait aof gc cron 

            assert_aof_meta_content $dir {
                {fileName appendonly.aof_b_4 fileSeq 4 fileType b}
            }

            assert_equal 1 [check_file_exist $dir "${aof_basename}${::base_aof_sufix}4"]
            assert_equal 0 [check_file_exist $dir "${aof_basename}${::incr_aof_sufix}6"]
            assert_equal 0 [check_file_exist $dir "${aof_basename}${::incr_aof_sufix}7"]

            set d1 [r debug digest]
            r debug loadaof
            set d2 [r debug digest]
            assert {$d1 eq $d2}

            r config set appendonly yes
            waitForBgrewriteaof r

            after 3000 ; # Wait aof gc cron 
           
            assert_aof_meta_content $dir {
                {fileName appendonly.aof_b_5 fileSeq 5 fileType b}
                {fileName appendonly.aof_i_1 fileSeq 1 fileType i}
            }

            assert_equal 0 [check_file_exist $dir "${aof_basename}${::base_aof_sufix}4"]
            assert_equal 1 [check_file_exist $dir "${aof_basename}${::base_aof_sufix}5"]
            assert_equal 1 [check_file_exist $dir "${aof_basename}${::incr_aof_sufix}1"]
        }

        test "AOF enable/disable aof auto gc" {
            r config set aof-enable-auto-gc no

            r bgrewriteaof
            waitForBgrewriteaof r

            r bgrewriteaof
            waitForBgrewriteaof r

            after 3000 ; # Wait aof gc cron 

            assert_aof_meta_content $dir {
                {fileName appendonly.aof_b_7 fileSeq 7 fileType b} 
                {fileName appendonly.aof_b_5 fileSeq 5 fileType h} 
                {fileName appendonly.aof_i_1 fileSeq 1 fileType h} 
                {fileName appendonly.aof_b_6 fileSeq 6 fileType h} 
                {fileName appendonly.aof_i_2 fileSeq 2 fileType h} 
                {fileName appendonly.aof_i_3 fileSeq 3 fileType i}
            }

            assert_equal 1 [check_file_exist $dir "${aof_basename}${::base_aof_sufix}5"]
            assert_equal 1 [check_file_exist $dir "${aof_basename}${::base_aof_sufix}6"]
            assert_equal 1 [check_file_exist $dir "${aof_basename}${::incr_aof_sufix}1"]
            assert_equal 1 [check_file_exist $dir "${aof_basename}${::incr_aof_sufix}2"]

            r config set aof-enable-auto-gc yes

            after 3000 ; # Wait aof gc cron 

            assert_aof_meta_content $dir {
                {fileName appendonly.aof_b_7 fileSeq 7 fileType b} 
                {fileName appendonly.aof_i_3 fileSeq 3 fileType i}
            }

            assert_equal 0 [check_file_exist $dir "${aof_basename}${::base_aof_sufix}5"]
            assert_equal 0 [check_file_exist $dir "${aof_basename}${::base_aof_sufix}6"]
            assert_equal 0 [check_file_exist $dir "${aof_basename}${::incr_aof_sufix}1"]
            assert_equal 0 [check_file_exist $dir "${aof_basename}${::incr_aof_sufix}2"]
        }

        test "AOF reload can recovery the sequence" {
            r debug loadaof

            r bgrewriteaof
            waitForBgrewriteaof r

            after 3000 ; # Wait aof gc cron 

            assert_aof_meta_content $dir {
                {fileName appendonly.aof_b_8 fileSeq 8 fileType b} 
                {fileName appendonly.aof_i_4 fileSeq 4 fileType i}
            }
        }

        test "AOF normally open a new INCR AOF when an rewrite is running" {
            r config set aof-child-rewrite-delay 1000000

            r bgrewriteaof

            assert_aof_meta_content $dir {
                {fileName appendonly.aof_b_8 fileSeq 8 fileType b} 
                {fileName appendonly.aof_i_4 fileSeq 4 fileType i}
                {fileName appendonly.aof_i_5 fileSeq 5 fileType i}
            }

            catch {r bgrewriteaof} err
            assert_match "*rewriting already in progress*" $err

            r config set appendonly no
            r config set appendonly yes
            r config set aof-child-rewrite-delay 0
            waitForBgrewriteaof r

            after 3000 ; # Wait aof gc cron 

            assert_aof_meta_content $dir {
                {fileName appendonly.aof_b_9 fileSeq 9 fileType b} 
                {fileName appendonly.aof_i_6 fileSeq 6 fileType i}
            }
        }

        test "AOF can load from old version redis data" {
            # r debug loadaof
        }
    }

    create_aof $base_1_aof_path {
        append_to_aof [formatCommand set k1 v1]
    }

    create_aof $incr_1_aof_path {
        append_to_aof [formatCommand set k2 v2]
    }

    create_aof $incr_3_aof_path {
        append_to_aof [formatCommand set k3 v3]
    }

    create_aof_meta $aof_meta_path {
        append_to_aofmeta "fileName appendonly.aof_b_1 fileSeq 1 fileType b\n"
        append_to_aofmeta "fileName appendonly.aof_i_1 fileSeq 1 fileType i\n"
        append_to_aofmeta "fileName appendonly.aof_i_3 fileSeq 3 fileType i\n"
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

    catch {exec rm -rf $incr_1_aof_path}
    start_server_aof [list dir $server_path] {
        test "AOF cannot load data when some aof missing" {
            assert_equal 0 [is_alive $srv]
        }
    }

    clean_aof_persistence $server_path

    create_aof $incr_1_aof_path {
        append_to_aof [formatCommand set k1 v1]
    }

    create_aof $incr_3_aof_path {
        append_to_aof [formatCommand set k2 v2]
    }

    create_aof_meta $aof_meta_path {
        append_to_aofmeta "fileName appendonly.aof_i_3 fileSeq 3 fileType i\n"
        append_to_aofmeta "fileName appendonly.aof_i_1 fileSeq 1 fileType i\n"
    }

    start_server_aof [list dir $server_path] {
        test "AOF cannot load data when the sequence not increase monotonically" {
            assert_equal 0 [is_alive $srv]
        }
    }

    clean_aof_persistence $server_path

    create_aof $incr_1_aof_path {
        append_to_aof [formatCommand set k1 v1]
    }

    create_aof $incr_3_aof_path {
        append_to_aof [formatCommand set k2 v2]
    }

    create_aof_meta $aof_meta_path {
        append_to_aofmeta "fileName appendonly.aof_i_1 fileSeq 1 fileType i\n"
        append_to_aofmeta "\n"
        append_to_aofmeta "#xxxxx\n"
        append_to_aofmeta "fileName appendonly.aof_i_3 fileSeq 3 fileType i\n"
    }

    start_server_aof [list dir $server_path] {
        test "AOF can load data when there are comments and blank lines in the meta file" {
            assert_equal 1 [is_alive $srv]
            set client [redis [dict get $srv host] [dict get $srv port] 0 $::tls]
            wait_done_loading $client

            assert_equal v1 [$client get k1]
            assert_equal v2 [$client get k2]
        }
    }

    catch {exec rm -rf $incr_1_aof_path}
    catch {exec rm -rf $incr_3_aof_path}
    catch {exec rm -rf $aof_meta_path}

    create_aof $base_1_aof_path {
        append_to_aof [formatCommand set k1 v1]
    }

    create_aof $base_2_aof_path {
        append_to_aof [formatCommand set k2 v2]
    }

    create_aof $incr_1_aof_path {
        append_to_aof [formatCommand set k3 v3]
    }

    create_aof_meta $aof_meta_path {
        append_to_aofmeta "fileName appendonly.aof_b_1 fileSeq 1 fileType b\n"
        append_to_aofmeta "fileName appendonly.aof_b_2 fileSeq 2 fileType b\n"
        append_to_aofmeta "fileName appendonly.aof_i_1 fileSeq 1 fileType i\n"
    }

    start_server_aof [list dir $server_path] {
        test "AOF cannot load data when there is a duplicate base aof" {
            assert_equal 0 [is_alive $srv]
        }
    }

    clean_aof_persistence $server_path

    create_aof $base_1_aof_path {
        append_to_aof [formatCommand set k1 v1]
    }

    create_aof $incr_1_aof_path {
        append_to_aof [formatCommand set k3 v3]
    }

    create_aof_meta $aof_meta_path {
        append_to_aofmeta "fileName appendonly.aof_b_1 fileSeq 1 fileType x\n"
        append_to_aofmeta "fileName appendonly.aof_i_1 fileSeq 1 fileType i\n"
    }

    start_server_aof [list dir $server_path] {
        test "AOF cannot load data when the meta format is wrong (fileType unknown)" {
            assert_equal 0 [is_alive $srv]
        }
    }

    clean_aof_persistence $server_path

    create_aof $base_1_aof_path {
        append_to_aof [formatCommand set k1 v1]
    }

    create_aof $incr_1_aof_path {
        append_to_aof [formatCommand set k3 v3]
    }

    create_aof_meta $aof_meta_path {
        append_to_aofmeta "filename appendonly.aof_b_1 fileSeq 1 fileType x\n"
        append_to_aofmeta "fileName appendonly.aof_i_1 fileSeq 1 fileType i\n"
    }

    start_server_aof [list dir $server_path] {
        test "AOF cannot load data when the meta format is wrong (fileName typo)" {
            assert_equal 0 [is_alive $srv]
        }
    }
}
