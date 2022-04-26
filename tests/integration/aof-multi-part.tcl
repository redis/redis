source tests/support/aofmanifest.tcl
set defaults {appendonly {yes} appendfilename {appendonly.aof} appenddirname {appendonlydir} auto-aof-rewrite-percentage {0}}
set server_path [tmpdir server.multi.aof]
set aof_dirname "appendonlydir"
set aof_basename "appendonly.aof"
set aof_dirpath "$server_path/$aof_dirname"
set aof_base1_file "$server_path/$aof_dirname/${aof_basename}.1$::base_aof_sufix$::aof_format_suffix"
set aof_base2_file "$server_path/$aof_dirname/${aof_basename}.2$::base_aof_sufix$::aof_format_suffix"
set aof_incr1_file "$server_path/$aof_dirname/${aof_basename}.1$::incr_aof_sufix$::aof_format_suffix"
set aof_incr2_file "$server_path/$aof_dirname/${aof_basename}.2$::incr_aof_sufix$::aof_format_suffix"
set aof_incr3_file "$server_path/$aof_dirname/${aof_basename}.3$::incr_aof_sufix$::aof_format_suffix"
set aof_manifest_file "$server_path/$aof_dirname/${aof_basename}$::manifest_suffix"
set aof_old_name_old_path "$server_path/$aof_basename"
set aof_old_name_new_path "$aof_dirpath/$aof_basename"
set aof_old_name_old_path2 "$server_path/${aof_basename}2"
set aof_manifest_file2 "$server_path/$aof_dirname/${aof_basename}2$::manifest_suffix"

tags {"external:skip"} {

    # Test Part 1

    # In order to test the loading logic of redis under different combinations of manifest and AOF.
    # We will manually construct the manifest file and AOF, and then start redis to verify whether
    # the redis behavior is as expected.

    test {Multi Part AOF can't load data when some file missing} {
        create_aof $aof_dirpath $aof_base1_file {
            append_to_aof [formatCommand set k1 v1]
        }

        create_aof $aof_dirpath $aof_incr2_file {
            append_to_aof [formatCommand set k2 v2]
        }

        create_aof_manifest $aof_dirpath $aof_manifest_file {
            append_to_manifest "file appendonly.aof.1.base.aof seq 1 type b\n"
            append_to_manifest "file appendonly.aof.1.incr.aof seq 1 type i\n"
            append_to_manifest "file appendonly.aof.2.incr.aof seq 2 type i\n"
        }

        start_server_aof [list dir $server_path] {
            wait_for_condition 100 50 {
                ! [is_alive $srv]
            } else {
                fail "AOF loading didn't fail"
            }

            assert_equal 1 [count_message_lines $server_path/stdout "appendonly.aof.1.incr.aof .*No such file or directory"]
        }

        clean_aof_persistence $aof_dirpath
    }

    test {Multi Part AOF can't load data when the sequence not increase monotonically} {
        create_aof $aof_dirpath $aof_incr1_file {
            append_to_aof [formatCommand set k1 v1]
        }

        create_aof $aof_dirpath $aof_incr2_file {
            append_to_aof [formatCommand set k2 v2]
        }

        create_aof_manifest $aof_dirpath $aof_manifest_file {
            append_to_manifest "file appendonly.aof.2.incr.aof seq 2 type i\n"
            append_to_manifest "file appendonly.aof.1.incr.aof seq 1 type i\n"
        }

        start_server_aof [list dir $server_path] {
            wait_for_condition 100 50 {
                ! [is_alive $srv]
            } else {
                fail "AOF loading didn't fail"
            }

            assert_equal 1 [count_message_lines $server_path/stdout "Found a non-monotonic sequence number"]
        }

        clean_aof_persistence $aof_dirpath
    }

    test {Multi Part AOF can't load data when there are blank lines in the manifest file} {
        create_aof $aof_dirpath $aof_incr1_file {
            append_to_aof [formatCommand set k1 v1]
        }

        create_aof $aof_dirpath $aof_incr3_file {
            append_to_aof [formatCommand set k2 v2]
        }

        create_aof_manifest $aof_dirpath $aof_manifest_file {
            append_to_manifest "file appendonly.aof.1.incr.aof seq 1 type i\n"
            append_to_manifest "\n"
            append_to_manifest "file appendonly.aof.3.incr.aof seq 3 type i\n"
        }

        start_server_aof [list dir $server_path] {
            wait_for_condition 100 50 {
                ! [is_alive $srv]
            } else {
                fail "AOF loading didn't fail"
            }

            assert_equal 1 [count_message_lines $server_path/stdout "Invalid AOF manifest file format"]
        }

        clean_aof_persistence $aof_dirpath
    }

    test {Multi Part AOF can't load data when there is a duplicate base file} {
        create_aof $aof_dirpath $aof_base1_file {
            append_to_aof [formatCommand set k1 v1]
        }

        create_aof $aof_dirpath $aof_base2_file {
            append_to_aof [formatCommand set k2 v2]
        }

        create_aof $aof_dirpath $aof_incr1_file {
            append_to_aof [formatCommand set k3 v3]
        }

        create_aof_manifest $aof_dirpath $aof_manifest_file {
            append_to_manifest "file appendonly.aof.1.base.aof seq 1 type b\n"
            append_to_manifest "file appendonly.aof.2.base.aof seq 2 type b\n"
            append_to_manifest "file appendonly.aof.1.incr.aof seq 1 type i\n"
        }

        start_server_aof [list dir $server_path] {
            wait_for_condition 100 50 {
                ! [is_alive $srv]
            } else {
                fail "AOF loading didn't fail"
            }

            assert_equal 1 [count_message_lines $server_path/stdout "Found duplicate base file information"]
        }

        clean_aof_persistence $aof_dirpath
    }

    test {Multi Part AOF can't load data when the manifest format is wrong (type unknown)} {
        create_aof $aof_dirpath $aof_base1_file {
            append_to_aof [formatCommand set k1 v1]
        }

        create_aof $aof_dirpath $aof_incr1_file {
            append_to_aof [formatCommand set k3 v3]
        }

        create_aof_manifest $aof_dirpath $aof_manifest_file {
            append_to_manifest "file appendonly.aof.1.base.aof seq 1 type x\n"
            append_to_manifest "file appendonly.aof.1.incr.aof seq 1 type i\n"
        }

        start_server_aof [list dir $server_path] {
            wait_for_condition 100 50 {
                ! [is_alive $srv]
            } else {
                fail "AOF loading didn't fail"
            }

            assert_equal 1 [count_message_lines $server_path/stdout "Unknown AOF file type"]
        }

        clean_aof_persistence $aof_dirpath
    }

    test {Multi Part AOF can't load data when the manifest format is wrong (missing key)} {
        create_aof $aof_dirpath $aof_base1_file {
            append_to_aof [formatCommand set k1 v1]
        }

        create_aof $aof_dirpath $aof_incr1_file {
            append_to_aof [formatCommand set k3 v3]
        }

        create_aof_manifest $aof_dirpath $aof_manifest_file {
            append_to_manifest "filx appendonly.aof.1.base.aof seq 1 type b\n"
            append_to_manifest "file appendonly.aof.1.incr.aof seq 1 type i\n"
        }

        start_server_aof [list dir $server_path] {
            wait_for_condition 100 50 {
                ! [is_alive $srv]
            } else {
                fail "AOF loading didn't fail"
            }

            assert_equal 2 [count_message_lines $server_path/stdout "Invalid AOF manifest file format"]
        }

        clean_aof_persistence $aof_dirpath
    }

    test {Multi Part AOF can't load data when the manifest format is wrong (line too short)} {
        create_aof $aof_dirpath $aof_base1_file {
            append_to_aof [formatCommand set k1 v1]
        }

        create_aof $aof_dirpath $aof_incr1_file {
            append_to_aof [formatCommand set k3 v3]
        }

        create_aof_manifest $aof_dirpath $aof_manifest_file {
            append_to_manifest "file appendonly.aof.1.base.aof seq 1 type b\n"
            append_to_manifest "file appendonly.aof.1.incr.aof type i\n"
        }

        start_server_aof [list dir $server_path] {
            wait_for_condition 100 50 {
                ! [is_alive $srv]
            } else {
                fail "AOF loading didn't fail"
            }

            assert_equal 3 [count_message_lines $server_path/stdout "Invalid AOF manifest file format"]
        }

        clean_aof_persistence $aof_dirpath
    }

    test {Multi Part AOF can't load data when the manifest format is wrong (line too long)} {
        create_aof $aof_dirpath $aof_base1_file {
            append_to_aof [formatCommand set k1 v1]
        }

        create_aof $aof_dirpath $aof_incr1_file {
            append_to_aof [formatCommand set k3 v3]
        }

        create_aof_manifest $aof_dirpath $aof_manifest_file {
            append_to_manifest "file appendonly.aof.1.base.aof seq 1 type b file appendonly.aof.1.base.aof seq 1 type b file appendonly.aof.1.base.aof seq 1 type b file appendonly.aof.1.base.aof seq 1 type b file appendonly.aof.1.base.aof seq 1 type b file appendonly.aof.1.base.aof seq 1 type b file appendonly.aof.1.base.aof seq 1 type b file appendonly.aof.1.base.aof seq 1 type b file appendonly.aof.1.base.aof seq 1 type b file appendonly.aof.1.base.aof seq 1 type b file appendonly.aof.1.base.aof seq 1 type b file appendonly.aof.1.base.aof seq 1 type b file appendonly.aof.1.base.aof seq 1 type b file appendonly.aof.1.base.aof seq 1 type b file appendonly.aof.1.base.aof seq 1 type b file appendonly.aof.1.base.aof seq 1 type b file appendonly.aof.1.base.aof seq 1 type b file appendonly.aof.1.base.aof seq 1 type b file appendonly.aof.1.base.aof seq 1 type b file appendonly.aof.1.base.aof seq 1 type b file appendonly.aof.1.base.aof seq 1 type b file appendonly.aof.1.base.aof seq 1 type b file appendonly.aof.1.base.aof seq 1 type b file appendonly.aof.1.base.aof seq 1 type b file appendonly.aof.1.base.aof seq 1 type b file appendonly.aof.1.base.aof seq 1 type b file appendonly.aof.1.base.aof seq 1 type b file appendonly.aof.1.base.aof seq 1 type b file appendonly.aof.1.base.aof seq 1 type b file appendonly.aof.1.base.aof seq 1 type b file appendonly.aof.1.base.aof seq 1 type b file appendonly.aof.1.base.aof seq 1 type b file appendonly.aof.1.base.aof seq 1 type b file appendonly.aof.1.base.aof seq 1 type b file appendonly.aof.1.base.aof seq 1 type b file appendonly.aof.1.base.aof seq 1 type b file appendonly.aof.1.base.aof seq 1 type b file appendonly.aof.1.base.aof seq 1 type b file appendonly.aof.1.base.aof seq 1 type b file appendonly.aof.1.base.aof seq 1 type b file appendonly.aof.1.base.aof seq 1 type b file appendonly.aof.1.base.aof seq 1 type b file appendonly.aof.1.base.aof seq 1 type b\n"
            append_to_manifest "file appendonly.aof.1.incr.aof seq 1 type i\n"
        }

        start_server_aof [list dir $server_path] {
            wait_for_condition 100 50 {
                ! [is_alive $srv]
            } else {
                fail "AOF loading didn't fail"
            }

            assert_equal 1 [count_message_lines $server_path/stdout "The AOF manifest file contains too long line"]
        }

        clean_aof_persistence $aof_dirpath
    }

    test {Multi Part AOF can't load data when the manifest format is wrong (odd parameter)} {
        create_aof $aof_dirpath $aof_base1_file {
            append_to_aof [formatCommand set k1 v1]
        }

        create_aof $aof_dirpath $aof_incr1_file {
            append_to_aof [formatCommand set k3 v3]
        }

        create_aof_manifest $aof_dirpath $aof_manifest_file {
            append_to_manifest "file appendonly.aof.1.base.aof seq 1 type b\n"
            append_to_manifest "file appendonly.aof.1.incr.aof seq 1 type i newkey\n"
        }

        start_server_aof [list dir $server_path] {
            wait_for_condition 100 50 {
                ! [is_alive $srv]
            } else {
                fail "AOF loading didn't fail"
            }

            assert_equal 4 [count_message_lines $server_path/stdout "Invalid AOF manifest file format"]
        }

        clean_aof_persistence $aof_dirpath
    }

    test {Multi Part AOF can't load data when the manifest file is empty} {
        create_aof_manifest $aof_dirpath $aof_manifest_file {
        }

        start_server_aof [list dir $server_path] {
            wait_for_condition 100 50 {
                ! [is_alive $srv]
            } else {
                fail "AOF loading didn't fail"
            }

            assert_equal 1 [count_message_lines $server_path/stdout "Found an empty AOF manifest"]
        }

        clean_aof_persistence $aof_dirpath
    }

    test {Multi Part AOF can start when no aof and no manifest} {
        start_server_aof [list dir $server_path] {
            assert_equal 1 [is_alive $srv]

            set client [redis [dict get $srv host] [dict get $srv port] 0 $::tls]

            assert_equal OK [$client set k1 v1]
            assert_equal v1 [$client get k1]
        }

        clean_aof_persistence $aof_dirpath
    }

    test {Multi Part AOF can start when we have en empty AOF dir} {
        create_aof_dir $aof_dirpath

        start_server_aof [list dir $server_path] {
            assert_equal 1 [is_alive $srv]
        }
    }

    test {Multi Part AOF can load data discontinuously increasing sequence} {
        create_aof $aof_dirpath $aof_base1_file {
            append_to_aof [formatCommand set k1 v1]
        }

        create_aof $aof_dirpath $aof_incr1_file {
            append_to_aof [formatCommand set k2 v2]
        }

        create_aof $aof_dirpath $aof_incr3_file {
            append_to_aof [formatCommand set k3 v3]
        }

        create_aof_manifest $aof_dirpath $aof_manifest_file {
            append_to_manifest "file appendonly.aof.1.base.aof seq 1 type b\n"
            append_to_manifest "file appendonly.aof.1.incr.aof seq 1 type i\n"
            append_to_manifest "file appendonly.aof.3.incr.aof seq 3 type i\n"
        }

        start_server_aof [list dir $server_path] {
            assert_equal 1 [is_alive $srv]
            set client [redis [dict get $srv host] [dict get $srv port] 0 $::tls]
            wait_done_loading $client

            assert_equal v1 [$client get k1]
            assert_equal v2 [$client get k2]
            assert_equal v3 [$client get k3]
        }

        clean_aof_persistence $aof_dirpath
    }

    test {Multi Part AOF can load data when manifest add new k-v} {
        create_aof $aof_dirpath $aof_base1_file {
            append_to_aof [formatCommand set k1 v1]
        }

        create_aof $aof_dirpath $aof_incr1_file {
            append_to_aof [formatCommand set k2 v2]
        }

        create_aof $aof_dirpath $aof_incr3_file {
            append_to_aof [formatCommand set k3 v3]
        }

        create_aof_manifest $aof_dirpath $aof_manifest_file {
            append_to_manifest "file appendonly.aof.1.base.aof seq 1 type b newkey newvalue\n"
            append_to_manifest "file appendonly.aof.1.incr.aof seq 1 type i\n"
            append_to_manifest "file appendonly.aof.3.incr.aof seq 3 type i\n"
        }

        start_server_aof [list dir $server_path] {
            assert_equal 1 [is_alive $srv]
            set client [redis [dict get $srv host] [dict get $srv port] 0 $::tls]
            wait_done_loading $client

            assert_equal v1 [$client get k1]
            assert_equal v2 [$client get k2]
            assert_equal v3 [$client get k3]
        }

        clean_aof_persistence $aof_dirpath
    }

    test {Multi Part AOF can load data when some AOFs are empty} {
        create_aof $aof_dirpath $aof_base1_file {
            append_to_aof [formatCommand set k1 v1]
        }

        create_aof $aof_dirpath $aof_incr1_file {
        }

        create_aof $aof_dirpath $aof_incr3_file {
            append_to_aof [formatCommand set k3 v3]
        }

        create_aof_manifest $aof_dirpath $aof_manifest_file {
            append_to_manifest "file appendonly.aof.1.base.aof seq 1 type b\n"
            append_to_manifest "file appendonly.aof.1.incr.aof seq 1 type i\n"
            append_to_manifest "file appendonly.aof.3.incr.aof seq 3 type i\n"
        }

        start_server_aof [list dir $server_path] {
            assert_equal 1 [is_alive $srv]
            set client [redis [dict get $srv host] [dict get $srv port] 0 $::tls]
            wait_done_loading $client

            assert_equal v1 [$client get k1]
            assert_equal "" [$client get k2]
            assert_equal v3 [$client get k3]
        }

        clean_aof_persistence $aof_dirpath
    }

    test {Multi Part AOF can load data from old version redis (rdb preamble no)} {
        create_aof $server_path $aof_old_name_old_path {
            append_to_aof [formatCommand set k1 v1]
            append_to_aof [formatCommand set k2 v2]
            append_to_aof [formatCommand set k3 v3]
        }

        start_server_aof [list dir $server_path] {
            assert_equal 1 [is_alive $srv]

            set client [redis [dict get $srv host] [dict get $srv port] 0 $::tls]
            wait_done_loading $client

            assert_equal v1 [$client get k1]
            assert_equal v2 [$client get k2]
            assert_equal v3 [$client get k3]

            assert_equal 0 [check_file_exist $server_path $aof_basename]
            assert_equal 1 [check_file_exist $aof_dirpath $aof_basename]

            assert_aof_manifest_content $aof_manifest_file  {
                {file appendonly.aof seq 1 type b}
                {file appendonly.aof.1.incr.aof seq 1 type i}
            }

            assert_equal OK [$client set k4 v4]

            $client bgrewriteaof
            waitForBgrewriteaof $client

            assert_equal OK [$client set k5 v5]

            assert_aof_manifest_content $aof_manifest_file {
                {file appendonly.aof.2.base.rdb seq 2 type b}
                {file appendonly.aof.2.incr.aof seq 2 type i}
            }

            set d1 [$client debug digest]
            $client debug loadaof
            set d2 [$client debug digest]
            assert {$d1 eq $d2}
        }

        clean_aof_persistence $aof_dirpath
    }

    test {Multi Part AOF can load data from old version redis (rdb preamble yes)} {
        exec cp tests/assets/rdb-preamble.aof $aof_old_name_old_path
        start_server_aof [list dir $server_path] {
            assert_equal 1 [is_alive $srv]

            set client [redis [dict get $srv host] [dict get $srv port] 0 $::tls]
            wait_done_loading $client

            # k1 k2 in rdb header and k3 in AOF tail
            assert_equal v1 [$client get k1]
            assert_equal v2 [$client get k2]
            assert_equal v3 [$client get k3]

            assert_equal 0 [check_file_exist $server_path $aof_basename]
            assert_equal 1 [check_file_exist $aof_dirpath $aof_basename]

            assert_aof_manifest_content $aof_manifest_file  {
                {file appendonly.aof seq 1 type b}
                {file appendonly.aof.1.incr.aof seq 1 type i}
            }

            assert_equal OK [$client set k4 v4]

            $client bgrewriteaof
            waitForBgrewriteaof $client

            assert_equal OK [$client set k5 v5]

            assert_aof_manifest_content $aof_manifest_file {
                {file appendonly.aof.2.base.rdb seq 2 type b}
                {file appendonly.aof.2.incr.aof seq 2 type i}
            }

            set d1 [$client debug digest]
            $client debug loadaof
            set d2 [$client debug digest]
            assert {$d1 eq $d2}
        }

        clean_aof_persistence $aof_dirpath
    }

    test {Multi Part AOF can continue the upgrade from the interrupted upgrade state} {
        create_aof $server_path $aof_old_name_old_path {
            append_to_aof [formatCommand set k1 v1]
            append_to_aof [formatCommand set k2 v2]
            append_to_aof [formatCommand set k3 v3]
        }

        # Create a layout of an interrupted upgrade (interrupted before the rename).
        create_aof_manifest $aof_dirpath $aof_manifest_file {
            append_to_manifest "file appendonly.aof seq 1 type b\n"
        }

        start_server_aof [list dir $server_path] {
            assert_equal 1 [is_alive $srv]

            set client [redis [dict get $srv host] [dict get $srv port] 0 $::tls]
            wait_done_loading $client

            assert_equal v1 [$client get k1]
            assert_equal v2 [$client get k2]
            assert_equal v3 [$client get k3]

            assert_equal 0 [check_file_exist $server_path $aof_basename]
            assert_equal 1 [check_file_exist $aof_dirpath $aof_basename]

            assert_aof_manifest_content $aof_manifest_file  {
                {file appendonly.aof seq 1 type b}
                {file appendonly.aof.1.incr.aof seq 1 type i}
            }
        }

        clean_aof_persistence $aof_dirpath
    }

    test {Multi Part AOF can be loaded correctly when both server dir and aof dir contain old AOF} {
        create_aof $server_path $aof_old_name_old_path {
            append_to_aof [formatCommand set k1 v1]
            append_to_aof [formatCommand set k2 v2]
            append_to_aof [formatCommand set k3 v3]
        }

        create_aof_manifest $aof_dirpath $aof_manifest_file {
            append_to_manifest "file appendonly.aof seq 1 type b\n"
        }

        create_aof $aof_dirpath $aof_old_name_new_path {
            append_to_aof [formatCommand set k4 v4]
            append_to_aof [formatCommand set k5 v5]
            append_to_aof [formatCommand set k6 v6]
        }

        start_server_aof [list dir $server_path] {
            assert_equal 1 [is_alive $srv]

            set client [redis [dict get $srv host] [dict get $srv port] 0 $::tls]
            wait_done_loading $client

            assert_equal 0 [$client exists k1]
            assert_equal 0 [$client exists k2]
            assert_equal 0 [$client exists k3]

            assert_equal v4 [$client get k4]
            assert_equal v5 [$client get k5]
            assert_equal v6 [$client get k6]

            assert_equal 1 [check_file_exist $server_path $aof_basename]
            assert_equal 1 [check_file_exist $aof_dirpath $aof_basename]

            assert_aof_manifest_content $aof_manifest_file  {
                {file appendonly.aof seq 1 type b}
                {file appendonly.aof.1.incr.aof seq 1 type i}
            }
        }

        clean_aof_persistence $aof_dirpath
        catch {exec rm -rf $aof_old_name_old_path}
    }

    test {Multi Part AOF can't load data when the manifest contains the old AOF file name but the file does not exist in server dir and aof dir} {
        create_aof_manifest $aof_dirpath $aof_manifest_file {
            append_to_manifest "file appendonly.aof seq 1 type b\n"
        }

        start_server_aof [list dir $server_path] {
            wait_for_condition 100 50 {
                ! [is_alive $srv]
            } else {
                fail "AOF loading didn't fail"
            }

            assert_equal 1 [count_message_lines $server_path/stdout "appendonly.aof .*No such file or directory"]
        }

        clean_aof_persistence $aof_dirpath
    }

    test {Multi Part AOF can upgrade when when two redis share the same server dir} {
        create_aof $server_path $aof_old_name_old_path {
            append_to_aof [formatCommand set k1 v1]
            append_to_aof [formatCommand set k2 v2]
            append_to_aof [formatCommand set k3 v3]
        }

        create_aof $server_path $aof_old_name_old_path2 {
            append_to_aof [formatCommand set k4 v4]
            append_to_aof [formatCommand set k5 v5]
            append_to_aof [formatCommand set k6 v6]
        }

        start_server_aof [list dir $server_path] {
            set redis1 [redis [dict get $srv host] [dict get $srv port] 0 $::tls]

            start_server [list overrides [list dir $server_path appendonly yes appendfilename appendonly.aof2]] {
                set redis2 [redis [srv host] [srv port] 0 $::tls]

                test "Multi Part AOF can upgrade when when two redis share the same server dir (redis1)" {
                    wait_done_loading $redis1
                    assert_equal v1 [$redis1 get k1]
                    assert_equal v2 [$redis1 get k2]
                    assert_equal v3 [$redis1 get k3]

                    assert_equal 0 [$redis1 exists k4]
                    assert_equal 0 [$redis1 exists k5]
                    assert_equal 0 [$redis1 exists k6]

                    assert_aof_manifest_content $aof_manifest_file  {
                        {file appendonly.aof seq 1 type b}
                        {file appendonly.aof.1.incr.aof seq 1 type i}
                    }

                    $redis1 bgrewriteaof
                    waitForBgrewriteaof $redis1

                    assert_equal OK [$redis1 set k v]

                    assert_aof_manifest_content $aof_manifest_file {
                        {file appendonly.aof.2.base.rdb seq 2 type b}
                        {file appendonly.aof.2.incr.aof seq 2 type i}
                    }

                    set d1 [$redis1 debug digest]
                    $redis1 debug loadaof
                    set d2 [$redis1 debug digest]
                    assert {$d1 eq $d2}
                }

                test "Multi Part AOF can upgrade when when two redis share the same server dir (redis2)" {
                    wait_done_loading $redis2

                    assert_equal 0 [$redis2 exists k1]
                    assert_equal 0 [$redis2 exists k2]
                    assert_equal 0 [$redis2 exists k3]

                    assert_equal v4 [$redis2 get k4]
                    assert_equal v5 [$redis2 get k5]
                    assert_equal v6 [$redis2 get k6]

                    assert_aof_manifest_content $aof_manifest_file2  {
                        {file appendonly.aof2 seq 1 type b}
                        {file appendonly.aof2.1.incr.aof seq 1 type i}
                    }

                    $redis2 bgrewriteaof
                    waitForBgrewriteaof $redis2

                    assert_equal OK [$redis2 set k v]

                    assert_aof_manifest_content $aof_manifest_file2 {
                        {file appendonly.aof2.2.base.rdb seq 2 type b}
                        {file appendonly.aof2.2.incr.aof seq 2 type i}
                    }

                    set d1 [$redis2 debug digest]
                    $redis2 debug loadaof
                    set d2 [$redis2 debug digest]
                    assert {$d1 eq $d2}
                }
            }
        }
    }

    test {Multi Part AOF can handle appendfilename contains whitespaces} {
        start_server [list overrides [list appendonly yes appendfilename "\" file seq \\n\\n.aof \""]] {
            set dir [get_redis_dir]
            set aof_manifest_name [format "%s/%s/%s%s" $dir "appendonlydir" " file seq \n\n.aof " $::manifest_suffix]
            set redis [redis [srv host] [srv port] 0 $::tls]

            assert_equal OK [$redis set k1 v1]

            $redis bgrewriteaof
            waitForBgrewriteaof $redis

            assert_aof_manifest_content $aof_manifest_name {
                {file " file seq \n\n.aof .2.base.rdb" seq 2 type b}
                {file " file seq \n\n.aof .2.incr.aof" seq 2 type i}
            }

            set d1 [$redis debug digest]
            $redis debug loadaof
            set d2 [$redis debug digest]
            assert {$d1 eq $d2}
        }

        clean_aof_persistence $aof_dirpath
    }

    test {Multi Part AOF can create BASE (RDB format) when redis starts from empty} {
        start_server_aof [list dir $server_path] {
            set client [redis [dict get $srv host] [dict get $srv port] 0 $::tls]
            wait_done_loading $client

            assert_equal 1 [check_file_exist $aof_dirpath "${aof_basename}.1${::base_aof_sufix}${::rdb_format_suffix}"]

            assert_aof_manifest_content $aof_manifest_file {
                {file appendonly.aof.1.base.rdb seq 1 type b}
                {file appendonly.aof.1.incr.aof seq 1 type i}
            }

            $client set foo behavior

            set d1 [$client debug digest]
            $client debug loadaof
            set d2 [$client debug digest]
            assert {$d1 eq $d2} 
        }

        clean_aof_persistence $aof_dirpath
    }

    test {Multi Part AOF can create BASE (AOF format) when redis starts from empty} {
        start_server_aof [list dir $server_path aof-use-rdb-preamble no] {
            set client [redis [dict get $srv host] [dict get $srv port] 0 $::tls]
            wait_done_loading $client

            assert_equal 1 [check_file_exist $aof_dirpath "${aof_basename}.1${::base_aof_sufix}${::aof_format_suffix}"]

            assert_aof_manifest_content $aof_manifest_file {
                {file appendonly.aof.1.base.aof seq 1 type b}
                {file appendonly.aof.1.incr.aof seq 1 type i}
            }

            $client set foo behavior

            set d1 [$client debug digest]
            $client debug loadaof
            set d2 [$client debug digest]
            assert {$d1 eq $d2} 
        }

        clean_aof_persistence $aof_dirpath
    }

    # Test Part 2
    #
    # To test whether the AOFRW behaves as expected during the redis run.
    # We will start redis first, then perform pressure writing, enable and disable AOF, and manually
    # and automatically run bgrewrite and other actions, to test whether the correct AOF file is created,
    # whether the correct manifest is generated, whether the data can be reload correctly under continuous
    # writing pressure, etc.


    start_server {tags {"Multi Part AOF"} overrides {aof-use-rdb-preamble {yes} appendonly {no}}} {
        set dir [get_redis_dir]
        set aof_basename "appendonly.aof"
        set aof_dirname "appendonlydir"
        set aof_dirpath "$dir/$aof_dirname"
        set aof_manifest_name "$aof_basename$::manifest_suffix"
        set aof_manifest_file "$dir/$aof_dirname/$aof_manifest_name"

        set master [srv 0 client]
        set master_host [srv 0 host]
        set master_port [srv 0 port]

        catch {exec rm -rf $aof_manifest_file}

        test "Make sure aof manifest $aof_manifest_name not in aof directory" {
            assert_equal 0 [file exists $aof_manifest_file]
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
            assert_aof_manifest_content $aof_manifest_file {
                {file appendonly.aof.1.base.rdb seq 1 type b}
                {file appendonly.aof.1.incr.aof seq 1 type i}
            }

            # Check we really have these files
            assert_equal 1 [check_file_exist $aof_dirpath $aof_manifest_name]
            assert_equal 1 [check_file_exist $aof_dirpath "${aof_basename}.1${::base_aof_sufix}${::rdb_format_suffix}"]
            assert_equal 1 [check_file_exist $aof_dirpath "${aof_basename}.1${::incr_aof_sufix}${::aof_format_suffix}"]

            r bgrewriteaof
            waitForBgrewriteaof r

            # The second AOFRW done
            assert_aof_manifest_content $aof_manifest_file {
                {file appendonly.aof.2.base.rdb seq 2 type b}
                {file appendonly.aof.2.incr.aof seq 2 type i}
            }

            assert_equal 1 [check_file_exist $aof_dirpath $aof_manifest_name]
            # Wait bio delete history
            wait_for_condition 1000 10 {
                [check_file_exist $aof_dirpath "${aof_basename}.1${::base_aof_sufix}${::rdb_format_suffix}"] == 0 &&
                [check_file_exist $aof_dirpath "${aof_basename}.1${::incr_aof_sufix}${::aof_format_suffix}"] == 0
            } else {
                fail "Failed to delete history AOF"
            }
            assert_equal 1 [check_file_exist $aof_dirpath "${aof_basename}.2${::base_aof_sufix}${::rdb_format_suffix}"]
            assert_equal 1 [check_file_exist $aof_dirpath "${aof_basename}.2${::incr_aof_sufix}${::aof_format_suffix}"]

            stop_write_load $load_handle0
            wait_load_handlers_disconnected

            set d1 [r debug digest]
            r debug loadaof
            set d2 [r debug digest]
            assert {$d1 eq $d2}
        }

        test "AOF multiple rewrite failures will open multiple INCR AOFs" {
            # Start write load
            r config set rdb-key-save-delay 10000000

            set orig_size [r dbsize]
            set load_handle0 [start_write_load $master_host $master_port 10]

            wait_for_condition 50 100 {
                [r dbsize] > $orig_size
            } else {
                fail "No write load detected."
            }

            # Let AOFRW fail three times
            r bgrewriteaof
            set pid1 [get_child_pid 0]
            catch {exec kill -9 $pid1}
            waitForBgrewriteaof r

            r bgrewriteaof
            set pid2 [get_child_pid 0]
            catch {exec kill -9 $pid2}
            waitForBgrewriteaof r

            r bgrewriteaof
            set pid3 [get_child_pid 0]
            catch {exec kill -9 $pid3}
            waitForBgrewriteaof r

            assert_equal 0 [check_file_exist $dir "temp-rewriteaof-bg-$pid1.aof"]
            assert_equal 0 [check_file_exist $dir "temp-rewriteaof-bg-$pid2.aof"]
            assert_equal 0 [check_file_exist $dir "temp-rewriteaof-bg-$pid3.aof"]

            # We will have four INCR AOFs
            assert_aof_manifest_content $aof_manifest_file {
                {file appendonly.aof.2.base.rdb seq 2 type b}
                {file appendonly.aof.2.incr.aof seq 2 type i}
                {file appendonly.aof.3.incr.aof seq 3 type i}
                {file appendonly.aof.4.incr.aof seq 4 type i}
                {file appendonly.aof.5.incr.aof seq 5 type i}
            }

            assert_equal 1 [check_file_exist $aof_dirpath "${aof_basename}.2${::base_aof_sufix}${::rdb_format_suffix}"]
            assert_equal 1 [check_file_exist $aof_dirpath "${aof_basename}.2${::incr_aof_sufix}${::aof_format_suffix}"]
            assert_equal 1 [check_file_exist $aof_dirpath "${aof_basename}.3${::incr_aof_sufix}${::aof_format_suffix}"]
            assert_equal 1 [check_file_exist $aof_dirpath "${aof_basename}.4${::incr_aof_sufix}${::aof_format_suffix}"]
            assert_equal 1 [check_file_exist $aof_dirpath "${aof_basename}.5${::incr_aof_sufix}${::aof_format_suffix}"]

            stop_write_load $load_handle0
            wait_load_handlers_disconnected

            set d1 [r debug digest]
            r debug loadaof
            set d2 [r debug digest]
            assert {$d1 eq $d2}

            r config set rdb-key-save-delay 0
            catch {exec kill -9 [get_child_pid 0]}
            wait_for_condition 1000 10 {
                [s rdb_bgsave_in_progress] eq 0
            } else {
                fail "bgsave did not stop in time"
            }

            # AOFRW success
            r bgrewriteaof
            waitForBgrewriteaof r

            # All previous INCR AOFs have become history
            # and have be deleted
            assert_aof_manifest_content $aof_manifest_file {
                {file appendonly.aof.3.base.rdb seq 3 type b}
                {file appendonly.aof.6.incr.aof seq 6 type i}
            }

            # Wait bio delete history
            wait_for_condition 1000 10 {
                [check_file_exist $aof_dirpath "${aof_basename}.2${::base_aof_sufix}${::rdb_format_suffix}"] == 0 &&
                [check_file_exist $aof_dirpath "${aof_basename}.2${::incr_aof_sufix}${::aof_format_suffix}"] == 0 &&
                [check_file_exist $aof_dirpath "${aof_basename}.3${::incr_aof_sufix}${::aof_format_suffix}"] == 0 &&
                [check_file_exist $aof_dirpath "${aof_basename}.4${::incr_aof_sufix}${::aof_format_suffix}"] == 0 &&
                [check_file_exist $aof_dirpath "${aof_basename}.5${::incr_aof_sufix}${::aof_format_suffix}"] == 0
            } else {
                fail "Failed to delete history AOF"
            }

            assert_equal 1 [check_file_exist $aof_dirpath "${aof_basename}.3${::base_aof_sufix}${::rdb_format_suffix}"]
            assert_equal 1 [check_file_exist $aof_dirpath "${aof_basename}.6${::incr_aof_sufix}${::aof_format_suffix}"]

            set d1 [r debug digest]
            r debug loadaof
            set d2 [r debug digest]
            assert {$d1 eq $d2}
        }

        test "AOF rewrite doesn't open new aof when AOF turn off" {
            r config set appendonly no

            r bgrewriteaof
            waitForBgrewriteaof r

            # We only have BASE AOF, no INCR AOF
            assert_aof_manifest_content $aof_manifest_file {
                {file appendonly.aof.4.base.rdb seq 4 type b}
            }

            assert_equal 1 [check_file_exist $aof_dirpath "${aof_basename}.4${::base_aof_sufix}${::rdb_format_suffix}"]
            wait_for_condition 1000 10 {
                [check_file_exist $aof_dirpath "${aof_basename}.6${::incr_aof_sufix}${::aof_format_suffix}"] == 0 &&
                [check_file_exist $aof_dirpath "${aof_basename}.7${::incr_aof_sufix}${::aof_format_suffix}"] == 0
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
            assert_aof_manifest_content $aof_manifest_file {
                {file appendonly.aof.5.base.rdb seq 5 type b}
                {file appendonly.aof.1.incr.aof seq 1 type i}
            }

            # Wait bio delete history
            wait_for_condition 1000 10 {
                [check_file_exist $aof_dirpath "${aof_basename}.4${::base_aof_sufix}${::rdb_format_suffix}"] == 0
            } else {
                fail "Failed to delete history AOF"
            }

            assert_equal 1 [check_file_exist $aof_dirpath "${aof_basename}.5${::base_aof_sufix}${::rdb_format_suffix}"]
            assert_equal 1 [check_file_exist $aof_dirpath "${aof_basename}.1${::incr_aof_sufix}${::aof_format_suffix}"]
        }

        test "AOF enable/disable auto gc" {
            r config set aof-disable-auto-gc yes

            r bgrewriteaof
            waitForBgrewriteaof r

            r bgrewriteaof
            waitForBgrewriteaof r

            # We can see four history AOFs (Evolved from two BASE and two INCR)
            assert_aof_manifest_content $aof_manifest_file {
                {file appendonly.aof.7.base.rdb seq 7 type b}
                {file appendonly.aof.2.incr.aof seq 2 type h}
                {file appendonly.aof.6.base.rdb seq 6 type h}
                {file appendonly.aof.1.incr.aof seq 1 type h}
                {file appendonly.aof.5.base.rdb seq 5 type h}
                {file appendonly.aof.3.incr.aof seq 3 type i}
            }

            assert_equal 1 [check_file_exist $aof_dirpath "${aof_basename}.5${::base_aof_sufix}${::rdb_format_suffix}"]
            assert_equal 1 [check_file_exist $aof_dirpath "${aof_basename}.6${::base_aof_sufix}${::rdb_format_suffix}"]
            assert_equal 1 [check_file_exist $aof_dirpath "${aof_basename}.1${::incr_aof_sufix}${::aof_format_suffix}"]
            assert_equal 1 [check_file_exist $aof_dirpath "${aof_basename}.2${::incr_aof_sufix}${::aof_format_suffix}"]

            r config set aof-disable-auto-gc no

            # Auto gc success
            assert_aof_manifest_content $aof_manifest_file {
                {file appendonly.aof.7.base.rdb seq 7 type b}
                {file appendonly.aof.3.incr.aof seq 3 type i}
            }

            # wait bio delete history
            wait_for_condition 1000 10 {
                [check_file_exist $aof_dirpath "${aof_basename}.5${::base_aof_sufix}${::rdb_format_suffix}"] == 0 &&
                [check_file_exist $aof_dirpath "${aof_basename}.6${::base_aof_sufix}${::rdb_format_suffix}"] == 0 &&
                [check_file_exist $aof_dirpath "${aof_basename}.1${::incr_aof_sufix}${::aof_format_suffix}"] == 0 &&
                [check_file_exist $aof_dirpath "${aof_basename}.2${::incr_aof_sufix}${::aof_format_suffix}"] == 0
            } else {
                fail "Failed to delete history AOF"
            }
        }

        test "AOF can produce consecutive sequence number after reload" {
            # Current manifest, BASE seq 7 and INCR seq 3
            assert_aof_manifest_content $aof_manifest_file {
                {file appendonly.aof.7.base.rdb seq 7 type b}
                {file appendonly.aof.3.incr.aof seq 3 type i}
            }

            r debug loadaof

            # Trigger AOFRW
            r bgrewriteaof
            waitForBgrewriteaof r

            # Now BASE seq is 8 and INCR seq is 4
            assert_aof_manifest_content $aof_manifest_file {
                {file appendonly.aof.8.base.rdb seq 8 type b}
                {file appendonly.aof.4.incr.aof seq 4 type i}
            }
        }

        test "AOF enable during BGSAVE will not write data util AOFRW finish" {
            r config set appendonly no
            r config set save ""
            r config set rdb-key-save-delay 10000000

            r set k1 v1
            r bgsave

            wait_for_condition 1000 10 {
                [s rdb_bgsave_in_progress] eq 1
            } else {
                fail "bgsave did not start in time"
            }

            # Make server.aof_rewrite_scheduled = 1
            r config set appendonly yes
            assert_equal [s aof_rewrite_scheduled] 1

            # Not open new INCR aof
            assert_aof_manifest_content $aof_manifest_file {
                {file appendonly.aof.8.base.rdb seq 8 type b}
                {file appendonly.aof.4.incr.aof seq 4 type i}
            }

            r set k2 v2
            r debug loadaof

            # Both k1 and k2 lost
            assert_equal 0 [r exists k1]
            assert_equal 0 [r exists k2]

            set total_forks [s total_forks]
            assert_equal [s rdb_bgsave_in_progress] 1
            r config set rdb-key-save-delay 0
            catch {exec kill -9 [get_child_pid 0]}
            wait_for_condition 1000 10 {
                [s rdb_bgsave_in_progress] eq 0
            } else {
                fail "bgsave did not stop in time"
            }

            # Make sure AOFRW was scheduled
            wait_for_condition 1000 10 {
                [s total_forks] == [expr $total_forks + 1]
            } else {
                fail "aof rewrite did not scheduled"
            }
            waitForBgrewriteaof r

            assert_aof_manifest_content $aof_manifest_file {
                {file appendonly.aof.9.base.rdb seq 9 type b}
                {file appendonly.aof.5.incr.aof seq 5 type i}
            }

            r set k3 v3
            r debug loadaof
            assert_equal v3 [r get k3]
        }

        test "AOF will trigger limit when AOFRW fails many times" {
            # Clear all data and trigger a successful AOFRW, so we can let 
            # server.aof_current_size equal to 0
            r flushall
            r bgrewriteaof
            waitForBgrewriteaof r

            r config set rdb-key-save-delay 10000000
            # Let us trigger AOFRW easily
            r config set auto-aof-rewrite-percentage 1
            r config set auto-aof-rewrite-min-size 1kb

            # Set a key so that AOFRW can be delayed
            r set k v

            # Let AOFRW fail 3 times, this will trigger AOFRW limit
            r bgrewriteaof
            catch {exec kill -9 [get_child_pid 0]}
            waitForBgrewriteaof r

            r bgrewriteaof
            catch {exec kill -9 [get_child_pid 0]}
            waitForBgrewriteaof r

            r bgrewriteaof
            catch {exec kill -9 [get_child_pid 0]}
            waitForBgrewriteaof r

            assert_aof_manifest_content $aof_manifest_file {
                {file appendonly.aof.10.base.rdb seq 10 type b}
                {file appendonly.aof.6.incr.aof seq 6 type i}
                {file appendonly.aof.7.incr.aof seq 7 type i}
                {file appendonly.aof.8.incr.aof seq 8 type i}
                {file appendonly.aof.9.incr.aof seq 9 type i}
            }
            
            # Write 1KB data to trigger AOFRW
            r set x [string repeat x 1024]

            # Make sure we have limit log
            wait_for_condition 1000 50 {
                [count_log_message 0 "triggered the limit"] == 1
            } else {
                fail "aof rewrite did not trigger limit"
            }
            assert_equal [status r aof_rewrite_in_progress] 0

            # No new INCR AOF be created
            assert_aof_manifest_content $aof_manifest_file {
                {file appendonly.aof.10.base.rdb seq 10 type b}
                {file appendonly.aof.6.incr.aof seq 6 type i}
                {file appendonly.aof.7.incr.aof seq 7 type i}
                {file appendonly.aof.8.incr.aof seq 8 type i}
                {file appendonly.aof.9.incr.aof seq 9 type i}
            }

            # Turn off auto rewrite
            r config set auto-aof-rewrite-percentage 0
            r config set rdb-key-save-delay 0
            catch {exec kill -9 [get_child_pid 0]}
            wait_for_condition 1000 10 {
                [s aof_rewrite_in_progress] eq 0
            } else {
                fail "aof rewrite did not stop in time"
            }

            # We can still manually execute AOFRW immediately
            r bgrewriteaof
            waitForBgrewriteaof r

            # Can create New INCR AOF
            assert_equal 1 [check_file_exist $aof_dirpath "${aof_basename}.10${::incr_aof_sufix}${::aof_format_suffix}"]

            assert_aof_manifest_content $aof_manifest_file {
                {file appendonly.aof.11.base.rdb seq 11 type b}
                {file appendonly.aof.10.incr.aof seq 10 type i}
            }

            set d1 [r debug digest]
            r debug loadaof
            set d2 [r debug digest]
            assert {$d1 eq $d2}
        }

        start_server {overrides {aof-use-rdb-preamble {yes} appendonly {no}}} {
            set dir [get_redis_dir]
            set aof_basename "appendonly.aof"
            set aof_dirname "appendonlydir"
            set aof_dirpath "$dir/$aof_dirname"
            set aof_manifest_name "$aof_basename$::manifest_suffix"
            set aof_manifest_file "$dir/$aof_dirname/$aof_manifest_name"

            set master [srv 0 client]
            set master_host [srv 0 host]
            set master_port [srv 0 port]

            test "AOF will open a temporary INCR AOF to accumulate data until the first AOFRW success when AOF is dynamically enabled" {
                r config set save ""
                # Increase AOFRW execution time to give us enough time to kill it
                r config set rdb-key-save-delay 10000000

                # Start write load
                set load_handle0 [start_write_load $master_host $master_port 10]

                wait_for_condition 50 100 {
                    [r dbsize] > 0
                } else {
                    fail "No write load detected."
                }

                # Enable AOF will trigger an initialized AOFRW
                r config set appendonly yes
                # Let AOFRW fail
                assert_equal 1 [s aof_rewrite_in_progress]
                set pid1 [get_child_pid 0]
                catch {exec kill -9 $pid1}
 
                # Wait for AOFRW to exit and delete temp incr aof
                wait_for_condition 1000 100 {
                    [count_log_message 0 "Removing the temp incr aof file"] == 1
                } else {
                    fail "temp aof did not delete"
                }

                # Make sure manifest file is not created
                assert_equal 0 [check_file_exist $aof_dirpath $aof_manifest_name]
                # Make sure BASE AOF is not created
                assert_equal 0 [check_file_exist $aof_dirpath "${aof_basename}.1${::base_aof_sufix}${::rdb_format_suffix}"]

                # Make sure the next AOFRW has started
                wait_for_condition 1000 50 {
                    [s aof_rewrite_in_progress] == 1
                } else {
                    fail "aof rewrite did not scheduled"
                }

                # Do a successful AOFRW
                set total_forks [s total_forks]
                r config set rdb-key-save-delay 0
                catch {exec kill -9 [get_child_pid 0]}

                # Make sure the next AOFRW has started
                wait_for_condition 1000 10 {
                    [s total_forks] == [expr $total_forks + 1]
                } else {
                    fail "aof rewrite did not scheduled"
                }
                waitForBgrewriteaof r

                assert_equal 2 [count_log_message 0 "Removing the temp incr aof file"]

                # BASE and INCR AOF are successfully created
                assert_aof_manifest_content $aof_manifest_file {
                    {file appendonly.aof.1.base.rdb seq 1 type b}
                    {file appendonly.aof.1.incr.aof seq 1 type i}
                }

                stop_write_load $load_handle0
                wait_load_handlers_disconnected

                set d1 [r debug digest]
                r debug loadaof
                set d2 [r debug digest]
                assert {$d1 eq $d2}

                # Dynamic disable AOF again
                r config set appendonly no

                # Disabling AOF does not delete previous AOF files
                r debug loadaof
                set d2 [r debug digest]
                assert {$d1 eq $d2}

                assert_equal 0 [s rdb_changes_since_last_save]
                r config set rdb-key-save-delay 10000000
                set load_handle0 [start_write_load $master_host $master_port 10]
                wait_for_condition 50 100 {
                    [s rdb_changes_since_last_save] > 0
                } else {
                    fail "No write load detected."
                }

                # Re-enable AOF
                r config set appendonly yes

                # Let AOFRW fail
                assert_equal 1 [s aof_rewrite_in_progress]
                set pid1 [get_child_pid 0]
                catch {exec kill -9 $pid1}

                # Wait for AOFRW to exit and delete temp incr aof
                wait_for_condition 1000 100 {
                    [count_log_message 0 "Removing the temp incr aof file"] == 3
                } else {
                    fail "temp aof did not delete 3 times"
                }

                # Make sure no new incr AOF was created           
                assert_aof_manifest_content $aof_manifest_file {
                    {file appendonly.aof.1.base.rdb seq 1 type b}
                    {file appendonly.aof.1.incr.aof seq 1 type i}
                }

                # Make sure the next AOFRW has started
                wait_for_condition 1000 50 {
                    [s aof_rewrite_in_progress] == 1
                } else {
                    fail "aof rewrite did not scheduled"
                }

                # Do a successful AOFRW
                set total_forks [s total_forks]
                r config set rdb-key-save-delay 0
                catch {exec kill -9 [get_child_pid 0]}

                wait_for_condition 1000 10 {
                    [s total_forks] == [expr $total_forks + 1]
                } else {
                    fail "aof rewrite did not scheduled"
                }
                waitForBgrewriteaof r

                assert_equal 4 [count_log_message 0 "Removing the temp incr aof file"]

                # New BASE and INCR AOF are successfully created
                assert_aof_manifest_content $aof_manifest_file {
                    {file appendonly.aof.2.base.rdb seq 2 type b}
                    {file appendonly.aof.2.incr.aof seq 2 type i}
                }

                stop_write_load $load_handle0
                wait_load_handlers_disconnected

                set d1 [r debug digest]
                r debug loadaof
                set d2 [r debug digest]
                assert {$d1 eq $d2}
            }
        }
    }
}
