tags {"rdb external:skip"} {

set server_path [tmpdir "server.rdb-encoding-test"]

# Copy RDB with different encodings in server path
exec cp tests/assets/encodings.rdb $server_path

start_server [list overrides [list "dir" $server_path "dbfilename" "encodings.rdb"]] {
  test "RDB encoding loading test" {
    r select 0
    csvdump r
  } {"0","compressible","string","aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
"0","hash","hash","a","1","aa","10","aaa","100","b","2","bb","20","bbb","200","c","3","cc","30","ccc","300","ddd","400","eee","5000000000",
"0","hash_zipped","hash","a","1","b","2","c","3",
"0","list","list","1","2","3","a","b","c","100000","6000000000","1","2","3","a","b","c","100000","6000000000","1","2","3","a","b","c","100000","6000000000",
"0","list_zipped","list","1","2","3","a","b","c","100000","6000000000",
"0","number","string","10"
"0","set","set","1","100000","2","3","6000000000","a","b","c",
"0","set_zipped_1","set","1","2","3","4",
"0","set_zipped_2","set","100000","200000","300000","400000",
"0","set_zipped_3","set","1000000000","2000000000","3000000000","4000000000","5000000000","6000000000",
"0","string","string","Hello World"
"0","zset","zset","a","1","b","2","c","3","aa","10","bb","20","cc","30","aaa","100","bbb","200","ccc","300","aaaa","1000","cccc","123456789","bbbb","5000000000",
"0","zset_zipped","zset","a","1","b","2","c","3",
}
}

set server_path [tmpdir "server.rdb-startup-test"]

start_server [list overrides [list "dir" $server_path] keep_persistence true] {
    test {Server started empty with non-existing RDB file} {
        r debug digest
    } {0000000000000000000000000000000000000000}
    # Save an RDB file, needed for the next test.
    r save
}

start_server [list overrides [list "dir" $server_path] keep_persistence true] {
    test {Server started empty with empty RDB file} {
        r debug digest
    } {0000000000000000000000000000000000000000}
}

start_server [list overrides [list "dir" $server_path] keep_persistence true] {
    test {Test RDB stream encoding} {
        for {set j 0} {$j < 1000} {incr j} {
            if {rand() < 0.9} {
                r xadd stream * foo $j
            } else {
                r xadd stream * bar $j
            }
        }
        r xgroup create stream mygroup 0
        set records [r xreadgroup GROUP mygroup Alice COUNT 2 STREAMS stream >]
        r xdel stream [lindex [lindex [lindex [lindex $records 0] 1] 1] 0]
        r xack stream mygroup [lindex [lindex [lindex [lindex $records 0] 1] 0] 0]
        set digest [r debug digest]
        r config set sanitize-dump-payload no
        r debug reload
        set newdigest [r debug digest]
        assert {$digest eq $newdigest}
    }
    test {Test RDB stream encoding - sanitize dump} {
        r config set sanitize-dump-payload yes
        r debug reload
        set newdigest [r debug digest]
        assert {$digest eq $newdigest}
    }
    # delete the stream, maybe valgrind will find something
    r del stream
}

# Helper function to start a server and kill it, just to check the error
# logged.
set defaults {}
proc start_server_and_kill_it {overrides code} {
    upvar defaults defaults srv srv server_path server_path
    set config [concat $defaults $overrides]
    set srv [start_server [list overrides $config keep_persistence true]]
    uplevel 1 $code
    kill_server $srv
}

# Make the RDB file unreadable
file attributes [file join $server_path dump.rdb] -permissions 0222

# Detect root account (it is able to read the file even with 002 perm)
set isroot 0
catch {
    open [file join $server_path dump.rdb]
    set isroot 1
}

# Now make sure the server aborted with an error
if {!$isroot} {
    start_server_and_kill_it [list "dir" $server_path] {
        test {Server should not start if RDB file can't be open} {
            wait_for_condition 50 100 {
                [string match {*Fatal error loading*} \
                    [exec tail -1 < [dict get $srv stdout]]]
            } else {
                fail "Server started even if RDB was unreadable!"
            }
        }
    }
}

# Fix permissions of the RDB file.
file attributes [file join $server_path dump.rdb] -permissions 0666

# Corrupt its CRC64 checksum.
set filesize [file size [file join $server_path dump.rdb]]
set fd [open [file join $server_path dump.rdb] r+]
fconfigure $fd -translation binary
seek $fd -8 end
puts -nonewline $fd "foobar00"; # Corrupt the checksum
close $fd

# Now make sure the server aborted with an error
start_server_and_kill_it [list "dir" $server_path] {
    test {Server should not start if RDB is corrupted} {
        wait_for_condition 50 100 {
            [string match {*CRC error*} \
                [exec tail -10 < [dict get $srv stdout]]]
        } else {
            fail "Server started even if RDB was corrupted!"
        }
    }
}

start_server {} {
    test {Test FLUSHALL aborts bgsave} {
        # 1000 keys with 1ms sleep per key should take 1 second
        r config set rdb-key-save-delay 1000
        r debug populate 1000
        r bgsave
        assert_equal [s rdb_bgsave_in_progress] 1
        r flushall
        # wait half a second max
        wait_for_condition 5 100 {
            [s rdb_bgsave_in_progress] == 0
        } else {
            fail "bgsave not aborted"
        }
        # veirfy that bgsave failed, by checking that the change counter is still high
        assert_lessthan 999 [s rdb_changes_since_last_save]
        # make sure the server is still writable
        r set x xx
    }

    test {bgsave resets the change counter} {
        r config set rdb-key-save-delay 0
        r bgsave
        wait_for_condition 50 100 {
            [s rdb_bgsave_in_progress] == 0
        } else {
            fail "bgsave not done"
        }
        assert_equal [s rdb_changes_since_last_save] 0
    }
}

test {client freed during loading} {
    start_server [list overrides [list key-load-delay 50 rdbcompression no]] {
        # create a big rdb that will take long to load. it is important
        # for keys to be big since the server processes events only once in 2mb.
        # 100mb of rdb, 100k keys will load in more than 5 seconds
        r debug populate 100000 key 1000

        restart_server 0 false false

        # make sure it's still loading
        assert_equal [s loading] 1

        # connect and disconnect 5 clients
        set clients {}
        for {set j 0} {$j < 5} {incr j} {
            lappend clients [redis_deferring_client]
        }
        foreach rd $clients {
            $rd debug log bla
        }
        foreach rd $clients {
            $rd read
        }
        foreach rd $clients {
            $rd close
        }

        # make sure the server freed the clients
        wait_for_condition 100 100 {
            [s connected_clients] < 3
        } else {
            fail "clients didn't disconnect"
        }

        # make sure it's still loading
        assert_equal [s loading] 1

        # no need to keep waiting for loading to complete
        exec kill [srv 0 pid]
    }
}

# Our COW metrics (Private_Dirty) work only on Linux
set system_name [string tolower [exec uname -s]]
if {$system_name eq {linux}} {

start_server {overrides {save ""}} {
    test {Test child sending info} {
        # make sure that rdb_last_cow_size and current_cow_size are zero (the test using new server),
        # so that the comparisons during the test will be valid
        assert {[s current_cow_size] == 0}
        assert {[s current_save_keys_processed] == 0}
        assert {[s current_save_keys_total] == 0}

        assert {[s rdb_last_cow_size] == 0}

        # using a 200us delay, the bgsave is empirically taking about 10 seconds.
        # we need it to take more than some 5 seconds, since redis only report COW once a second.
        r config set rdb-key-save-delay 200
        r config set loglevel debug

        # populate the db with 10k keys of 4k each
        set rd [redis_deferring_client 0]
        set size 4096
        set cmd_count 10000
        for {set k 0} {$k < $cmd_count} {incr k} {
            $rd set key$k [string repeat A $size]
        }

        for {set k 0} {$k < $cmd_count} {incr k} {
            catch { $rd read }
        }

        $rd close

        # start background rdb save
        r bgsave

        set current_save_keys_total [s current_save_keys_total]
        if {$::verbose} {
            puts "Keys before bgsave start: current_save_keys_total"
        }

        # on each iteration, we will write some key to the server to trigger copy-on-write, and
        # wait to see that it reflected in INFO.
        set iteration 1
        while 1 {
            # take samples before writing new data to the server
            set cow_size [s current_cow_size]
            if {$::verbose} {
                puts "COW info before copy-on-write: $cow_size"
            }

            set keys_processed [s current_save_keys_processed]
            if {$::verbose} {
                puts "current_save_keys_processed info : $keys_processed"
            }

            # trigger copy-on-write
            r setrange key$iteration 0 [string repeat B $size]

            # wait to see that current_cow_size value updated (as long as the child is in progress)
            wait_for_condition 80 100 {
                [s rdb_bgsave_in_progress] == 0 ||
                [s current_cow_size] >= $cow_size + $size && 
                [s current_save_keys_processed] > $keys_processed &&
                [s current_fork_perc] > 0
            } else {
                if {$::verbose} {
                    puts "COW info on fail: [s current_cow_size]"
                    puts [exec tail -n 100 < [srv 0 stdout]]
                }
                fail "COW info wasn't reported"
            }

            # assert that $keys_processed is not greater than total keys.
            assert_morethan_equal $current_save_keys_total $keys_processed

            # for no accurate, stop after 2 iterations
            if {!$::accurate && $iteration == 2} {
                break
            }

            # stop iterating if the bgsave completed
            if { [s rdb_bgsave_in_progress] == 0 } {
                break
            }

            incr iteration 1
        }

        # make sure we saw report of current_cow_size
        if {$iteration < 2 && $::verbose} {
            puts [exec tail -n 100 < [srv 0 stdout]]
        }
        assert_morethan_equal $iteration 2

        # if bgsave completed, check that rdb_last_cow_size (fork exit report)
        # is at least 90% of last rdb_active_cow_size.
        if { [s rdb_bgsave_in_progress] == 0 } {
            set final_cow [s rdb_last_cow_size]
            set cow_size [expr $cow_size * 0.9]
            if {$final_cow < $cow_size && $::verbose} {
                puts [exec tail -n 100 < [srv 0 stdout]]
            }
            assert_morethan_equal $final_cow $cow_size
        }
    }
}
} ;# system_name

} ;# tags
