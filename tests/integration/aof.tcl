source tests/support/aofmanifest.tcl
set defaults { appendonly {yes} appendfilename {appendonly.aof} appenddirname {appendonlydir} auto-aof-rewrite-percentage {0}}
set server_path [tmpdir server.aof]
set aof_dirname "appendonlydir"
set aof_basename "appendonly.aof"
set aof_dirpath "$server_path/$aof_dirname"
set aof_base_file "$server_path/$aof_dirname/${aof_basename}.1$::base_aof_sufix$::aof_format_suffix"
set aof_file "$server_path/$aof_dirname/${aof_basename}.1$::incr_aof_sufix$::aof_format_suffix"
set aof_manifest_file "$server_path/$aof_dirname/$aof_basename$::manifest_suffix"

tags {"aof external:skip"} {
    # Server can start when aof-load-truncated is set to yes and AOF
    # is truncated, with an incomplete MULTI block.
    create_aof $aof_dirpath $aof_file {
        append_to_aof [formatCommand set foo hello]
        append_to_aof [formatCommand multi]
        append_to_aof [formatCommand set bar world]
    }

    create_aof_manifest $aof_dirpath $aof_manifest_file {
        append_to_manifest "file appendonly.aof.1.incr.aof seq 1 type i\n"
    }

    start_server_aof [list dir $server_path aof-load-truncated yes] {
        test "Unfinished MULTI: Server should start if load-truncated is yes" {
            assert_equal 1 [is_alive $srv]
        }
    }

    ## Should also start with truncated AOF without incomplete MULTI block.
    create_aof $aof_dirpath $aof_file {
        append_to_aof [formatCommand incr foo]
        append_to_aof [formatCommand incr foo]
        append_to_aof [formatCommand incr foo]
        append_to_aof [formatCommand incr foo]
        append_to_aof [formatCommand incr foo]
        append_to_aof [string range [formatCommand incr foo] 0 end-1]
    }

    start_server_aof [list dir $server_path aof-load-truncated yes] {
        test "Short read: Server should start if load-truncated is yes" {
            assert_equal 1 [is_alive $srv]
        }

        test "Truncated AOF loaded: we expect foo to be equal to 5" {
            set client [redis [dict get $srv host] [dict get $srv port] 0 $::tls]
            wait_done_loading $client
            assert {[$client get foo] eq "5"}
        }

        test "Append a new command after loading an incomplete AOF" {
            $client incr foo
        }
    }

    # Now the AOF file is expected to be correct
    start_server_aof [list dir $server_path aof-load-truncated yes] {
        test "Short read + command: Server should start" {
            assert_equal 1 [is_alive $srv]
        }

        test "Truncated AOF loaded: we expect foo to be equal to 6 now" {
            set client [redis [dict get $srv host] [dict get $srv port] 0 $::tls]
            wait_done_loading $client
            assert {[$client get foo] eq "6"}
        }
    }

    ## Test that the server exits when the AOF contains a format error
    create_aof $aof_dirpath $aof_file {
        append_to_aof [formatCommand set foo hello]
        append_to_aof "!!!"
        append_to_aof [formatCommand set foo hello]
    }

    start_server_aof [list dir $server_path aof-load-truncated yes] {
        test "Bad format: Server should have logged an error" {
            set pattern "*Bad file format reading the append only file*"
            set retry 10
            while {$retry} {
                set result [exec tail -1 < [dict get $srv stdout]]
                if {[string match $pattern $result]} {
                    break
                }
                incr retry -1
                after 1000
            }
            if {$retry == 0} {
                error "assertion:expected error not found on config file"
            }
        }
    }

    ## Test the server doesn't start when the AOF contains an unfinished MULTI
    create_aof $aof_dirpath $aof_file {
        append_to_aof [formatCommand set foo hello]
        append_to_aof [formatCommand multi]
        append_to_aof [formatCommand set bar world]
    }

    start_server_aof [list dir $server_path aof-load-truncated no] {
        test "Unfinished MULTI: Server should have logged an error" {
            set pattern "*Unexpected end of file reading the append only file*"
            set retry 10
            while {$retry} {
                set result [exec tail -1 < [dict get $srv stdout]]
                if {[string match $pattern $result]} {
                    break
                }
                incr retry -1
                after 1000
            }
            if {$retry == 0} {
                error "assertion:expected error not found on config file"
            }
        }
    }

    ## Test that the server exits when the AOF contains a short read
    create_aof $aof_dirpath $aof_file {
        append_to_aof [formatCommand set foo hello]
        append_to_aof [string range [formatCommand set bar world] 0 end-1]
    }

    start_server_aof [list dir $server_path aof-load-truncated no] {
        test "Short read: Server should have logged an error" {
            set pattern "*Unexpected end of file reading the append only file*"
            set retry 10
            while {$retry} {
                set result [exec tail -1 < [dict get $srv stdout]]
                if {[string match $pattern $result]} {
                    break
                }
                incr retry -1
                after 1000
            }
            if {$retry == 0} {
                error "assertion:expected error not found on config file"
            }
        }
    }

    ## Test that redis-check-aof indeed sees this AOF is not valid
    test "Short read: Utility should confirm the AOF is not valid" {
        catch {
            exec src/redis-check-aof $aof_manifest_file
        } result
        assert_match "*not valid*" $result
    }

    test "Short read: Utility should show the abnormal line num in AOF" {
        create_aof $aof_dirpath $aof_file {
            append_to_aof [formatCommand set foo hello]
            append_to_aof "!!!"
        }

        catch {
            exec src/redis-check-aof $aof_manifest_file
        } result
        assert_match "*ok_up_to_line=8*" $result
    }

    test "Short read: Utility should be able to fix the AOF" {
        set result [exec src/redis-check-aof --fix $aof_manifest_file << "y\n"]
        assert_match "*Successfully truncated AOF*" $result
    }

    ## Test that the server can be started using the truncated AOF
    start_server_aof [list dir $server_path aof-load-truncated no] {
        test "Fixed AOF: Server should have been started" {
            assert_equal 1 [is_alive $srv]
        }

        test "Fixed AOF: Keyspace should contain values that were parseable" {
            set client [redis [dict get $srv host] [dict get $srv port] 0 $::tls]
            wait_done_loading $client
            assert_equal "hello" [$client get foo]
            assert_equal "" [$client get bar]
        }
    }

    ## Test that SPOP (that modifies the client's argc/argv) is correctly free'd
    create_aof $aof_dirpath $aof_file {
        append_to_aof [formatCommand sadd set foo]
        append_to_aof [formatCommand sadd set bar]
        append_to_aof [formatCommand spop set]
    }

    start_server_aof [list dir $server_path aof-load-truncated no] {
        test "AOF+SPOP: Server should have been started" {
            assert_equal 1 [is_alive $srv]
        }

        test "AOF+SPOP: Set should have 1 member" {
            set client [redis [dict get $srv host] [dict get $srv port] 0 $::tls]
            wait_done_loading $client
            assert_equal 1 [$client scard set]
        }
    }

    ## Uses the alsoPropagate() API.
    create_aof $aof_dirpath $aof_file {
        append_to_aof [formatCommand sadd set foo]
        append_to_aof [formatCommand sadd set bar]
        append_to_aof [formatCommand sadd set gah]
        append_to_aof [formatCommand spop set 2]
    }

    start_server_aof [list dir $server_path] {
        test "AOF+SPOP: Server should have been started" {
            assert_equal 1 [is_alive $srv]
        }

        test "AOF+SPOP: Set should have 1 member" {
            set client [redis [dict get $srv host] [dict get $srv port] 0 $::tls]
            wait_done_loading $client
            assert_equal 1 [$client scard set]
        }
    }

    ## Test that PEXPIREAT is loaded correctly
    create_aof $aof_dirpath $aof_file {
        append_to_aof [formatCommand rpush list foo]
        append_to_aof [formatCommand pexpireat list 1000]
        append_to_aof [formatCommand rpush list bar]
    }

    start_server_aof [list dir $server_path aof-load-truncated no] {
        test "AOF+EXPIRE: Server should have been started" {
            assert_equal 1 [is_alive $srv]
        }

        test "AOF+EXPIRE: List should be empty" {
            set client [redis [dict get $srv host] [dict get $srv port] 0 $::tls]
            wait_done_loading $client
            assert_equal 0 [$client llen list]
        }
    }

    start_server {overrides {appendonly {yes}}} {
        test {Redis should not try to convert DEL into EXPIREAT for EXPIRE -1} {
            r set x 10
            r expire x -1
        }
    }

    start_server {overrides {appendonly {yes} appendfsync always}} {
        test {AOF fsync always barrier issue} {
            set rd [redis_deferring_client]
            # Set a sleep when aof is flushed, so that we have a chance to look
            # at the aof size and detect if the response of an incr command
            # arrives before the data was written (and hopefully fsynced)
            # We create a big reply, which will hopefully not have room in the
            # socket buffers, and will install a write handler, then we sleep
            # a big and issue the incr command, hoping that the last portion of
            # the output buffer write, and the processing of the incr will happen
            # in the same event loop cycle.
            # Since the socket buffers and timing are unpredictable, we fuzz this
            # test with slightly different sizes and sleeps a few times.
            for {set i 0} {$i < 10} {incr i} {
                r debug aof-flush-sleep 0
                r del x
                r setrange x [expr {int(rand()*5000000)+10000000}] x
                r debug aof-flush-sleep 500000
                set aof [get_last_incr_aof_path r]
                set size1 [file size $aof]
                $rd get x
                after [expr {int(rand()*30)}]
                $rd incr new_value
                $rd read
                $rd read
                set size2 [file size $aof]
                assert {$size1 != $size2}
            }
        }
    }

    start_server {overrides {appendonly {yes}}} {
        test {GETEX should not append to AOF} {
            set aof [get_last_incr_aof_path r]
            r set foo bar
            set before [file size $aof]
            r getex foo
            set after [file size $aof]
            assert_equal $before $after
        }
    }

    ## Test that the server exits when the AOF contains a unknown command
    create_aof $aof_dirpath $aof_file {
        append_to_aof [formatCommand set foo hello]
        append_to_aof [formatCommand bla foo hello]
        append_to_aof [formatCommand set foo hello]
    }

    start_server_aof [list dir $server_path aof-load-truncated yes] {
        test "Unknown command: Server should have logged an error" {
            set pattern "*Unknown command 'bla' reading the append only file*"
            set retry 10
            while {$retry} {
                set result [exec tail -1 < [dict get $srv stdout]]
                if {[string match $pattern $result]} {
                    break
                }
                incr retry -1
                after 1000
            }
            if {$retry == 0} {
                error "assertion:expected error not found on config file"
            }
        }
    }

    # Test that LMPOP/BLMPOP work fine with AOF.
    create_aof $aof_dirpath $aof_file {
        append_to_aof [formatCommand lpush mylist a b c]
        append_to_aof [formatCommand rpush mylist2 1 2 3]
        append_to_aof [formatCommand lpush mylist3 a b c d e]
    }

    start_server_aof [list dir $server_path aof-load-truncated no] {
        test "AOF+LMPOP/BLMPOP: pop elements from the list" {
            set client [redis [dict get $srv host] [dict get $srv port] 0 $::tls]
            set client2 [redis [dict get $srv host] [dict get $srv port] 1 $::tls]
            wait_done_loading $client

            # Pop all elements from mylist, should be blmpop delete mylist.
            $client lmpop 1 mylist left count 1
            $client blmpop 0 1 mylist left count 10

            # Pop all elements from mylist2, should be lmpop delete mylist2.
            $client blmpop 0 2 mylist mylist2 right count 10
            $client lmpop 2 mylist mylist2 right count 2

            # Blocking path, be blocked and then released.
            $client2 blmpop 0 2 mylist mylist2 left count 2
            after 100
            $client lpush mylist2 a b c

            # Pop up the last element in mylist2
            $client blmpop 0 3 mylist mylist2 mylist3 left count 1

            # Leave two elements in mylist3.
            $client blmpop 0 3 mylist mylist2 mylist3 right count 3
        }
    }

    start_server_aof [list dir $server_path aof-load-truncated no] {
        test "AOF+LMPOP/BLMPOP: after pop elements from the list" {
            set client [redis [dict get $srv host] [dict get $srv port] 0 $::tls]
            wait_done_loading $client

            # mylist and mylist2 no longer exist.
            assert_equal 0 [$client exists mylist mylist2]

            # Length of mylist3 is two.
            assert_equal 2 [$client llen mylist3]
        }
    }

    # Test that ZMPOP/BZMPOP work fine with AOF.
    create_aof $aof_dirpath $aof_file {
        append_to_aof [formatCommand zadd myzset 1 one 2 two 3 three]
        append_to_aof [formatCommand zadd myzset2 4 four 5 five 6 six]
        append_to_aof [formatCommand zadd myzset3 1 one 2 two 3 three 4 four 5 five]
    }

    start_server_aof [list dir $server_path aof-load-truncated no] {
        test "AOF+ZMPOP/BZMPOP: pop elements from the zset" {
            set client [redis [dict get $srv host] [dict get $srv port] 0 $::tls]
            set client2 [redis [dict get $srv host] [dict get $srv port] 1 $::tls]
            wait_done_loading $client

            # Pop all elements from myzset, should be bzmpop delete myzset.
            $client zmpop 1 myzset min count 1
            $client bzmpop 0 1 myzset min count 10

            # Pop all elements from myzset2, should be zmpop delete myzset2.
            $client bzmpop 0 2 myzset myzset2 max count 10
            $client zmpop 2 myzset myzset2 max count 2

            # Blocking path, be blocked and then released.
            $client2 bzmpop 0 2 myzset myzset2 min count 2
            after 100
            $client zadd myzset2 1 one 2 two 3 three

            # Pop up the last element in myzset2
            $client bzmpop 0 3 myzset myzset2 myzset3 min count 1

            # Leave two elements in myzset3.
            $client bzmpop 0 3 myzset myzset2 myzset3 max count 3
        }
    }

    start_server_aof [list dir $server_path aof-load-truncated no] {
        test "AOF+ZMPOP/BZMPOP: after pop elements from the zset" {
            set client [redis [dict get $srv host] [dict get $srv port] 0 $::tls]
            wait_done_loading $client

            # myzset and myzset2 no longer exist.
            assert_equal 0 [$client exists myzset myzset2]

            # Length of myzset3 is two.
            assert_equal 2 [$client zcard myzset3]
        }
    }

    test {Generate timestamp annotations in AOF} {
        start_server {overrides {appendonly {yes}}} {
            r config set aof-timestamp-enabled yes
            r config set aof-use-rdb-preamble no
            set aof [get_last_incr_aof_path r]

            r set foo bar
            assert_match "#TS:*" [exec head -n 1 $aof]

            r bgrewriteaof
            waitForBgrewriteaof r

            set aof [get_base_aof_path r]
            assert_match "#TS:*" [exec head -n 1 $aof]
        }
    }

    # redis could load AOF which has timestamp annotations inside
    create_aof $aof_dirpath $aof_file {
        append_to_aof "#TS:1628217470\r\n"
        append_to_aof [formatCommand set foo1 bar1]
        append_to_aof "#TS:1628217471\r\n"
        append_to_aof [formatCommand set foo2 bar2]
        append_to_aof "#TS:1628217472\r\n"
        append_to_aof "#TS:1628217473\r\n"
        append_to_aof [formatCommand set foo3 bar3]
        append_to_aof "#TS:1628217474\r\n"
    }
    start_server_aof [list dir $server_path] {
        test {Successfully load AOF which has timestamp annotations inside} {
            set c [redis [dict get $srv host] [dict get $srv port] 0 $::tls]
            wait_done_loading $c
            assert_equal "bar1" [$c get foo1]
            assert_equal "bar2" [$c get foo2]
            assert_equal "bar3" [$c get foo3]
        }
    }

    test {Truncate AOF to specific timestamp} {
        # truncate to timestamp 1628217473
        exec src/redis-check-aof --truncate-to-timestamp 1628217473 $aof_manifest_file
        start_server_aof [list dir $server_path] {
            set c [redis [dict get $srv host] [dict get $srv port] 0 $::tls]
            wait_done_loading $c
            assert_equal "bar1" [$c get foo1]
            assert_equal "bar2" [$c get foo2]
            assert_equal "bar3" [$c get foo3]
        }

        # truncate to timestamp 1628217471
        exec src/redis-check-aof --truncate-to-timestamp 1628217471 $aof_manifest_file
        start_server_aof [list dir $server_path] {
            set c [redis [dict get $srv host] [dict get $srv port] 0 $::tls]
            wait_done_loading $c
            assert_equal "bar1" [$c get foo1]
            assert_equal "bar2" [$c get foo2]
            assert_equal "" [$c get foo3]
        }

        # truncate to timestamp 1628217470
        exec src/redis-check-aof --truncate-to-timestamp 1628217470 $aof_manifest_file
        start_server_aof [list dir $server_path] {
            set c [redis [dict get $srv host] [dict get $srv port] 0 $::tls]
            wait_done_loading $c
            assert_equal "bar1" [$c get foo1]
            assert_equal "" [$c get foo2]
        }

        # truncate to timestamp 1628217469
        catch {exec src/redis-check-aof --truncate-to-timestamp 1628217469 $aof_manifest_file} e
        assert_match {*aborting*} $e
    }

    test {EVAL timeout with slow verbatim Lua script from AOF} {
        start_server [list overrides [list dir $server_path appendonly yes lua-time-limit 1 aof-use-rdb-preamble no]] {  
            # generate a long running script that is propagated to the AOF as script
            # make sure that the script times out during loading
            create_aof $aof_dirpath $aof_file {
                append_to_aof [formatCommand select 9]
                append_to_aof [formatCommand eval {redis.call('set',KEYS[1],'y'); for i=1,1500000 do redis.call('ping') end return 'ok'} 1 x]
            }
            set rd [redis_deferring_client]
            $rd debug loadaof
            $rd flush
            wait_for_condition 100 10 {
                [catch {r ping} e] == 1
            } else {
                fail "server didn't start loading"
            }
            assert_error {LOADING*} {r ping}
            $rd read
            $rd close
            wait_for_log_messages 0 {"*Slow script detected*"} 0 100 100
            assert_equal [r get x] y
        }
    }

    test {EVAL can process writes from AOF in read-only replicas} {
        create_aof_manifest $aof_dirpath $aof_manifest_file {
            append_to_manifest "file appendonly.aof.1.incr.aof seq 1 type i\n"
        }
        create_aof $aof_dirpath $aof_file {
            append_to_aof [formatCommand select 9]
            append_to_aof [formatCommand eval {redis.call("set",KEYS[1],"100")} 1 foo]
            append_to_aof [formatCommand eval {redis.call("incr",KEYS[1])} 1 foo]
            append_to_aof [formatCommand eval {redis.call("incr",KEYS[1])} 1 foo]
        }
        start_server [list overrides [list dir $server_path appendonly yes replica-read-only yes replicaof "127.0.0.1 0"]] {
            assert_equal [r get foo] 102
        }
    }

    test {Test redis-check-aof for old style resp AOF} {
        create_aof $aof_dirpath $aof_file {
            append_to_aof [formatCommand set foo hello]
            append_to_aof [formatCommand set bar world]
        }

        catch {
            exec src/redis-check-aof $aof_file
        } result
        assert_match "*Start checking Old-Style AOF*is valid*" $result
    }

    test {Test redis-check-aof for old style rdb-preamble AOF} {
        catch {
            exec src/redis-check-aof tests/assets/rdb-preamble.aof
        } result
        assert_match "*Start checking Old-Style AOF*RDB preamble is OK, proceeding with AOF tail*is valid*" $result
    }

    test {Test redis-check-aof for Multi Part AOF with resp AOF base} {
        create_aof $aof_dirpath $aof_base_file {
            append_to_aof [formatCommand set foo hello]
            append_to_aof [formatCommand set bar world]
        }

        create_aof $aof_dirpath $aof_file {
            append_to_aof [formatCommand set foo hello]
            append_to_aof [formatCommand set bar world]
        }

        create_aof_manifest $aof_dirpath $aof_manifest_file {
            append_to_manifest "file appendonly.aof.1.base.aof seq 1 type b\n"
            append_to_manifest "file appendonly.aof.1.incr.aof seq 1 type i\n"
        }

        catch {
            exec src/redis-check-aof $aof_manifest_file
        } result   
        assert_match "*Start checking Multi Part AOF*Start to check BASE AOF (RESP format)*BASE AOF*is valid*Start to check INCR files*INCR AOF*is valid*All AOF files and manifest are valid*" $result
    }

    test {Test redis-check-aof for Multi Part AOF with rdb-preamble AOF base} {
        exec cp tests/assets/rdb-preamble.aof $aof_base_file

        create_aof $aof_dirpath $aof_file {
            append_to_aof [formatCommand set foo hello]
            append_to_aof [formatCommand set bar world]
        }

        create_aof_manifest $aof_dirpath $aof_manifest_file {
            append_to_manifest "file appendonly.aof.1.base.aof seq 1 type b\n"
            append_to_manifest "file appendonly.aof.1.incr.aof seq 1 type i\n"
        }

        catch {
            exec src/redis-check-aof $aof_manifest_file
        } result
        assert_match "*Start checking Multi Part AOF*Start to check BASE AOF (RDB format)*DB preamble is OK, proceeding with AOF tail*BASE AOF*is valid*Start to check INCR files*INCR AOF*is valid*All AOF files and manifest are valid*" $result
    }

    test {Test redis-check-aof only truncates the last file for Multi Part AOF in fix mode} {
        create_aof $aof_dirpath $aof_base_file {
            append_to_aof [formatCommand set foo hello]
            append_to_aof [formatCommand multi]
            append_to_aof [formatCommand set bar world]
        }

        create_aof $aof_dirpath $aof_file {
            append_to_aof [formatCommand set foo hello]
            append_to_aof [formatCommand set bar world]
        }

        create_aof_manifest $aof_dirpath $aof_manifest_file {
            append_to_manifest "file appendonly.aof.1.base.aof seq 1 type b\n"
            append_to_manifest "file appendonly.aof.1.incr.aof seq 1 type i\n"
        }

        catch {
            exec src/redis-check-aof $aof_manifest_file 
        } result
        assert_match "*not valid*" $result

        catch {
            exec src/redis-check-aof --fix $aof_manifest_file 
        } result
        assert_match "*Failed to truncate AOF*because it is not the last file*" $result
    }

    test {Test redis-check-aof only truncates the last file for Multi Part AOF in truncate-to-timestamp mode} {
        create_aof $aof_dirpath $aof_base_file {
            append_to_aof "#TS:1628217470\r\n"
            append_to_aof [formatCommand set foo1 bar1]
            append_to_aof "#TS:1628217471\r\n"
            append_to_aof [formatCommand set foo2 bar2]
            append_to_aof "#TS:1628217472\r\n"
            append_to_aof "#TS:1628217473\r\n"
            append_to_aof [formatCommand set foo3 bar3]
            append_to_aof "#TS:1628217474\r\n"
        }

        create_aof $aof_dirpath $aof_file {
            append_to_aof [formatCommand set foo hello]
            append_to_aof [formatCommand set bar world]
        }

        create_aof_manifest $aof_dirpath $aof_manifest_file {
            append_to_manifest "file appendonly.aof.1.base.aof seq 1 type b\n"
            append_to_manifest "file appendonly.aof.1.incr.aof seq 1 type i\n"
        }

        catch {
            exec src/redis-check-aof --truncate-to-timestamp 1628217473 $aof_manifest_file 
        } result
        assert_match "*Failed to truncate AOF*to timestamp*because it is not the last file*" $result
    }
}
