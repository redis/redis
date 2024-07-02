tags {"rdb external:skip"} {

set server_path [tmpdir "server.rdb-encoding-test"]

# Copy RDB with different encodings in server path
exec cp tests/assets/encodings.rdb $server_path
exec cp tests/assets/list-quicklist.rdb $server_path

start_server [list overrides [list "dir" $server_path "dbfilename" "list-quicklist.rdb" save ""]] {
    test "test old version rdb file" {
        r select 0
        assert_equal [r get x] 7
        assert_encoding listpack list
        r lpop list
    } {7}
}

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
        debug_digest
    } {0000000000000000000000000000000000000000}
    # Save an RDB file, needed for the next test.
    r save
}

start_server [list overrides [list "dir" $server_path] keep_persistence true] {
    test {Server started empty with empty RDB file} {
        debug_digest
    } {0000000000000000000000000000000000000000}
}

start_server [list overrides [list "dir" $server_path] keep_persistence true] {
    test {Test RDB stream encoding} {
        for {set j 0} {$j < 1000} {incr j} {
            if {rand() < 0.9} {
                r xadd stream * foo abc
            } else {
                r xadd stream * bar $j
            }
        }
        r xgroup create stream mygroup 0
        set records [r xreadgroup GROUP mygroup Alice COUNT 2 STREAMS stream >]
        r xdel stream [lindex [lindex [lindex [lindex $records 0] 1] 1] 0]
        r xack stream mygroup [lindex [lindex [lindex [lindex $records 0] 1] 0] 0]
        set digest [debug_digest]
        r config set sanitize-dump-payload no
        r debug reload
        set newdigest [debug_digest]
        assert {$digest eq $newdigest}
    }
    test {Test RDB stream encoding - sanitize dump} {
        r config set sanitize-dump-payload yes
        r debug reload
        set newdigest [debug_digest]
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
        r config set save ""
        # 5000 keys with 1ms sleep per key should take 5 second
        r config set rdb-key-save-delay 1000
        populate 5000
        assert_lessthan 999 [s rdb_changes_since_last_save]
        r bgsave
        assert_equal [s rdb_bgsave_in_progress] 1
        r flushall
        # wait a second max (bgsave should take 5)
        wait_for_condition 10 100 {
            [s rdb_bgsave_in_progress] == 0
        } else {
            fail "bgsave not aborted"
        }
        # verify that bgsave failed, by checking that the change counter is still high
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
    start_server [list overrides [list key-load-delay 50 loading-process-events-interval-bytes 1024 rdbcompression no save "900 1"]] {
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

start_server {} {
    test {Test RDB load info} {
        r debug populate 1000
        r save
        assert {[r lastsave] <= [lindex [r time] 0]}
        restart_server 0 true false
        wait_done_loading r
        assert {[s rdb_last_load_keys_expired] == 0}
        assert {[s rdb_last_load_keys_loaded] == 1000}

        r debug set-active-expire 0
        for {set j 0} {$j < 1024} {incr j} {
            r select [expr $j%16]
            r set $j somevalue px 10
        }
        after 20

        r save
        restart_server 0 true false
        wait_done_loading r
        assert {[s rdb_last_load_keys_expired] == 1024}
        assert {[s rdb_last_load_keys_loaded] == 1000}
    }
}

# Our COW metrics (Private_Dirty) work only on Linux
set system_name [string tolower [exec uname -s]]
set page_size [exec getconf PAGESIZE]
if {$system_name eq {linux} && $page_size == 4096} {

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

        # populate the db with 10k keys of 512B each (since we want to measure the COW size by
        # changing some keys and read the reported COW size, we are using small key size to prevent from
        # the "dismiss mechanism" free memory and reduce the COW size)
        set rd [redis_deferring_client 0]
        set size 500 ;# aim for the 512 bin (sds overhead)
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
            puts "Keys before bgsave start: $current_save_keys_total"
        }

        # on each iteration, we will write some key to the server to trigger copy-on-write, and
        # wait to see that it reflected in INFO.
        set iteration 1
        set key_idx 0
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
            set modified_keys 16
            for {set k 0} {$k < $modified_keys} {incr k} {
                r setrange key$key_idx 0 [string repeat B $size]
                incr key_idx 1
            }

            # changing 16 keys (512B each) will create at least 8192 COW (2 pages), but we don't want the test
            # to be too strict, so we check for a change of at least 4096 bytes
            set exp_cow [expr $cow_size + 4096]
            # wait to see that current_cow_size value updated (as long as the child is in progress)
            wait_for_condition 80 100 {
                [s rdb_bgsave_in_progress] == 0 ||
                [s current_cow_size] >= $exp_cow &&
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

exec cp -f tests/assets/scriptbackup.rdb $server_path
start_server [list overrides [list "dir" $server_path "dbfilename" "scriptbackup.rdb" "appendonly" "no"]] {
    # the script is: "return redis.call('set', 'foo', 'bar')""
    # its sha1   is: a0c38691e9fffe4563723c32ba77a34398e090e6
    test {script won't load anymore if it's in rdb} {
        assert_equal [r script exists a0c38691e9fffe4563723c32ba77a34398e090e6] 0
    }
}

start_server {} {
    test "failed bgsave prevents writes" {
        # Make sure the server saves an RDB on shutdown
        r config set save "900 1"

        r config set rdb-key-save-delay 10000000
        populate 1000
        r set x x
        r bgsave
        set pid1 [get_child_pid 0]
        catch {exec kill -9 $pid1}
        waitForBgsave r

        # make sure a read command succeeds
        assert_equal [r get x] x

        # make sure a write command fails
        assert_error {MISCONF *} {r set x y}

        # repeate with script
        assert_error {MISCONF *} {r eval {
            return redis.call('set','x',1)
            } 1 x
        }
        assert_equal {x} [r eval {
            return redis.call('get','x')
            } 1 x
        ]

        # again with script using shebang
        assert_error {MISCONF *} {r eval {#!lua
            return redis.call('set','x',1)
            } 1 x
        }
        assert_equal {x} [r eval {#!lua flags=no-writes
            return redis.call('get','x')
            } 1 x
        ]

        r config set rdb-key-save-delay 0
        r bgsave
        waitForBgsave r

        # server is writable again
        r set x y
    } {OK}
}

set server_path [tmpdir "server.partial-hfield-exp-test"]

# verifies writing and reading hash key with expiring and persistent fields
start_server [list overrides [list "dir" $server_path]] {
    foreach {type lp_entries} {listpack 512 dict 0} {
        test "HFE - save and load expired fields, expired soon after, or long after ($type)" {
            r config set hash-max-listpack-entries $lp_entries

            r FLUSHALL

            r HMSET key a 1 b 2 c 3 d 4 e 5
            # expected to be expired long after restart
            r HEXPIREAT key 2524600800 FIELDS 1 a
            # expected long TTL value (46 bits) is saved and loaded correctly
            r HPEXPIREAT key 65755674080852 FIELDS 1 b
            # expected to be already expired after restart
            r HPEXPIRE key 80 FIELDS 1 d
            # expected to be expired soon after restart
            r HPEXPIRE key 200 FIELDS 1 e

            r save
            # sleep 101 ms to make sure d will expire after restart
            after 101
            restart_server 0 true false
            wait_done_loading r

            # Never be sure when active-expire kicks in into action
            wait_for_condition 100 10 {
                [lsort [r hgetall key]] == "1 2 3 a b c"
            } else {
                fail "hgetall of key is not as expected"
            }

            assert_equal [r hpexpiretime key FIELDS 3 a b c] {2524600800000 65755674080852 -1}
            assert_equal [s rdb_last_load_keys_loaded] 1

            # wait until expired_subkeys equals 2
            wait_for_condition 10 100 {
                [s expired_subkeys] == 2
            } else {
                fail "Value of expired_subkeys is not as expected"
            }
        }
    }
}

set server_path [tmpdir "server.all-hfield-exp-test"]

# verifies writing hash with several expired keys, and active-expiring it on load
start_server [list overrides [list "dir" $server_path]] {
    foreach {type lp_entries} {listpack 512 dict 0} {
        test "HFE - save and load rdb all fields expired, ($type)" {
            r config set hash-max-listpack-entries $lp_entries

            r FLUSHALL

            r HMSET key a 1 b 2 c 3 d 4
            r HPEXPIRE key 100 FIELDS 4 a b c d

            r save
            # sleep 101 ms to make sure all fields will expire after restart
            after 101

            restart_server 0 true false
            wait_done_loading r

            #  it is expected that no field was expired on load and the key was
            # loaded, even though all its fields are actually expired.
            assert_equal [s rdb_last_load_keys_loaded] 1

            assert_equal [r hgetall key] {}
        }
    }
}

set server_path [tmpdir "server.listpack-to-dict-test"]

test "save listpack, load dict" {
    start_server [list overrides [list "dir" $server_path  enable-debug-command yes]] {
        r config set hash-max-listpack-entries 512

        r FLUSHALL

        r HMSET key a 1 b 2 c 3 d 4
        assert_match "*encoding:listpack*" [r debug object key]
        r HPEXPIRE key 100 FIELDS 1 d
        r save

        # sleep 200 ms to make sure 'd' will expire after when reloading
        after 200

        # change configuration and reload - result should be dict-encoded key
        r config set hash-max-listpack-entries 0
        r debug reload nosave

        # first verify d was not expired during load (no expiry when loading
        # a hash that was saved listpack-encoded)
        assert_equal [s rdb_last_load_keys_loaded] 1

        # d should be lazy expired in hgetall
        assert_equal [lsort [r hgetall key]] "1 2 3 a b c"
        assert_match "*encoding:hashtable*" [r debug object key]
    }
}

set server_path [tmpdir "server.dict-to-listpack-test"]

test "save dict, load listpack" {
    start_server [list overrides [list "dir" $server_path  enable-debug-command yes]] {
        r config set hash-max-listpack-entries 0

        r FLUSHALL

        r HMSET key a 1 b 2 c 3 d 4
        assert_match "*encoding:hashtable*" [r debug object key]
        r HPEXPIRE key 200 FIELDS 1 d
        r save

        # sleep 201 ms to make sure 'd' will expire during reload
        after 201

        # change configuration and reload - result should be LP-encoded key
        r config set hash-max-listpack-entries 512
        r debug reload nosave

        # verify d was expired during load
        assert_equal [s rdb_last_load_keys_loaded] 1

        assert_equal [lsort [r hgetall key]] "1 2 3 a b c"
        assert_match "*encoding:listpack*" [r debug object key]
    }
}

set server_path [tmpdir "server.active-expiry-after-load"]

# verifies a field is correctly expired by active expiry AFTER loading from RDB
foreach {type lp_entries} {listpack 512 dict 0} {
    start_server [list overrides [list "dir" $server_path enable-debug-command yes]] {
        test "active field expiry after load, ($type)" {
            r config set hash-max-listpack-entries $lp_entries

            r FLUSHALL

            r HMSET key a 1 b 2 c 3 d 4 e 5 f 6
            r HEXPIREAT key 2524600800 FIELDS 2 a b
            r HPEXPIRE key 200 FIELDS 2 c d

            r save
            r debug reload nosave

            # wait at most 2 secs to make sure 'c' and 'd' will active-expire
            wait_for_condition 20 100 {
                [s expired_subkeys] == 2
            } else {
                fail "expired hash fields is [s expired_subkeys] != 2"
            }

            assert_equal [s rdb_last_load_keys_loaded] 1

            # hgetall might lazy expire fields, so it's only called after the stat asserts
            assert_equal [lsort [r hgetall key]] "1 2 5 6 a b e f"
            assert_equal [r hexpiretime key FIELDS 6 a b c d e f] {2524600800 2524600800 -2 -2 -1 -1}
        }
    }
}

set server_path [tmpdir "server.lazy-expiry-after-load"]

foreach {type lp_entries} {listpack 512 dict 0} {
    start_server [list overrides [list "dir" $server_path enable-debug-command yes]] {
        test "lazy field expiry after load, ($type)" {
            r config set hash-max-listpack-entries $lp_entries
            r debug set-active-expire 0

            r FLUSHALL

            r HMSET key a 1 b 2 c 3 d 4 e 5 f 6
            r HEXPIREAT key 2524600800 FIELDS 2 a b
            r HPEXPIRE key 200 FIELDS 2 c d

            r save
            r debug reload nosave

            # sleep 500 msec to make sure 'c' and 'd' will lazy-expire when calling hgetall
            after 500

            assert_equal [s rdb_last_load_keys_loaded] 1
            assert_equal [s expired_subkeys] 0

            # hgetall will lazy expire fields, so it's only called after the stat asserts
            assert_equal [lsort [r hgetall key]] "1 2 5 6 a b e f"
            assert_equal [r hexpiretime key FIELDS 6 a b c d e f] {2524600800 2524600800 -2 -2 -1 -1}
        }
    }
}

set server_path [tmpdir "server.unexpired-items-rax-list-boundary"]

foreach {type lp_entries} {listpack 512 dict 0} {
    start_server [list overrides [list "dir" $server_path enable-debug-command yes]] {
        test "load un-expired items below and above rax-list boundary, ($type)" {
            r config set hash-max-listpack-entries $lp_entries

            r flushall

            set hash_sizes {15 16 17 31 32 33}
            foreach h $hash_sizes {
                for {set i 1} {$i <= $h} {incr i} {
                    r hset key$h f$i v$i
                    r hexpireat key$h 2524600800 FIELDS 1 f$i
                }
            }

            r save

            restart_server 0 true false
            wait_done_loading r

            set hash_sizes {15 16 17 31 32 33}
            foreach h $hash_sizes {
                for {set i 1} {$i <= $h} {incr i} {
                    # random expiration time
                    assert_equal [r hget key$h f$i] v$i
                    assert_equal [r hexpiretime key$h FIELDS 1 f$i] 2524600800
                }
            }
        }
    }
}

} ;# tags
