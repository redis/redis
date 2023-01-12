start_server {overrides {save ""} tags {"swap" "select"}} {
    test {multi-db basics} {
        r select 0
        r set key1 db0
        wait_key_cold r key1
        r select 1
        r set key1 db1
        wait_key_cold r key1

        r select 0
        assert_equal [r get key1] db0
        r select 1
        assert_equal [r get key1] db1

        r select 0
        r del key1
        assert_equal [r get key1] {}
        r select 1
        assert_equal [r get key1] db1

        r del key1
        assert_equal [r get key1] {}
        r select 0
        assert_equal [r get key1] {}
    }
    
    test {multi-db expire} {
        r select 0
        r set key2 db0
        r select 1
        r set key2 db1

        r select 0
        assert_equal [r get key2] db0
        r pexpire key2 1
        r select 1
        assert_equal [r get key2] db1
        r pexpire key2 1

        after 1000

        r select 0
        assert_equal [r get key2] {}
        r select 1
        assert_equal [r get key2] {}
    }

    test {multi-db multi/exec} {
        r select 0
        r set key3 db0
        r hmset myhash f1 v1 f2 v2 f3 v3
        r select 1
        r set key3 db1
        r rpush mylist 1 2 3 4 5

        wait_keyspace_cold r

        r select 0

        r multi
        r select 1
        r lindex mylist 3
        r hmget myhash f1 f2
        r select 0
        r get key3
        r hmget myhash f1 f2
        r select 1
        r get key3

        assert_equal [r exec] {OK 4 {{} {}} OK db0 {v1 v2} OK db1}

        r flushall
    }

    test {multi-db eval: select not allowed} {
        catch {r eval {redis.call('select', '0')} 0} e
        assert_match {*select command is not allowed from scripts in disk mode*} $e
    }

    test {multi-db metascan} {
    }

    test {multi-db scanexpire} {
        r select 0
        r setex key5 1 db0
        r select 1
        r setex key5 1 db1

        wait_keyspace_cold r

        after 2000

        r select 0
        assert_equal [rio_get_meta r key5] {}
        assert_equal [r get key5] {}
        r select 1
        assert_equal [rio_get_meta r key5] {}
        assert_equal [r get key5] {}
    }

    test {multi-db gtid} {
        r select 3
        r gtid A:1 1 set key6 db1
        r gtid A:2 0 set key6 db0 
        r select 0
        r hmset myhash6 f1 db0 f2 db0
        r gtid A:3 1 rpush mylist6 db1-1 db1-2 db1-3 db1-4
        r gtid A:4 0 rpush mylist6 db0-1 db0-2 db0-3 db0-4
        r select 1
        r hmset myhash6 f1 db1 f2 db1

        # wait_keyspace_cold r #TODO uncomment when list ready
        r select 0
        wait_key_cold r key6
        wait_key_cold r myhash6
        r select 1
        wait_key_cold r key6
        wait_key_cold r myhash6

        r select 0
        assert_equal [r get key6] db0
        assert_equal [r lindex mylist6 2] db0-3
        assert_equal [r hmget myhash6 f1 f2] {db0 db0}

        r select 1
        assert_equal [r get key6] db1
        assert_equal [r lindex mylist6 2] db1-3
        assert_equal [r hmget myhash6 f1 f2] {db1 db1}

        r flushall
    }

    test {multi-db gtid exec} {
        r multi
        r gtid A:11 1 set key6 db1
        r gtid A:12 0 set key6 db0 
        r select 0
        r hmset myhash6 f1 db0 f2 db0
        r gtid A:13 1 rpush mylist6 db1-1 db1-2 db1-3 db1-4
        r gtid A:14 0 rpush mylist6 db0-1 db0-2 db0-3 db0-4
        r select 1
        r hmset myhash6 f1 db1 f2 db1
        r gtid A:10 1 exec

        # wait_keyspace_cold r #TODO uncomment when list ready
        r select 0
        wait_key_cold r key6
        wait_key_cold r myhash6
        r select 1
        wait_key_cold r key6
        wait_key_cold r myhash6

        r select 0
        assert_equal [r get key6] db0
        assert_equal [r lindex mylist6 2] db0-3
        assert_equal [r hmget myhash6 f1 f2] {db0 db0}

        r select 1
        assert_equal [r get key6] db1
        assert_equal [r lindex mylist6 2] db1-3
        assert_equal [r hmget myhash6 f1 f2] {db1 db1}

        r flushall
    }
}

start_server {overrides {save ""} tags {"swap" "select"}} {
    start_server {} {
        set master [srv -1 client]
        set master_host [srv -1 host]
        set master_port [srv -1 port]
        set slave [srv 0 client]

        test {multi-db repl by rdb} {
            $master config set swap-debug-evict-keys 0

            for {set db 0} {$db < 5} {incr db} {
                $master select $db
                for {set count 0} {$count < 100} {incr count} {
                    $master set kv-$count db-$db 
                    if {$count < 50} {
                        $master swap.evict kv-$count
                    }
                }

                for {set count 0} {$count < 100} {incr count} {
                    $master hmset hash-$count f1 db-$db f2 db-$db f3 db-$db f4 db-$db f5 db-$db f6 db-$db f7 db-$db f8 db-$db

                    if {$count < 25} {
                        $master config set swap-evict-step-max-subkeys 1024
                        $master swap.evict hash-$count
                    }

                    if {$count < 50} {
                        $master config set swap-evict-step-max-subkeys 4
                        $master swap.evict hash-$count 
                    }
                }
            }

            after 1000

            $slave slaveof $master_host $master_port
            wait_for_sync $slave

            for {set db 0} {$db < 5} {incr db} {
                $slave select $db
                for {set count 0} {$count < 100} {incr count} {
                    assert_equal [$slave get kv-$count] db-$db
                }

                for {set count 0} {$count < 100} {incr count} {
                    assert_equal [$slave hmget hash-$count f1 f8] [list db-$db db-$db]
                }
            }
        }

        test {multi-db propagate expire} {
            $master select 1
            $master setex key 1 db1
            $master select 0
            $master setex key 1 db0

            wait_for_ofs_sync $master $slave

            $slave select 0
            assert_equal [$slave get key] db0
            $slave select 1
            assert_equal [$slave get key] db1

            after 1000

            $slave select 0
            assert_equal [$slave get key] {}
            $slave select 1
            assert_equal [$slave get key] {}
        }

        test {multi-db multi/exec} {
            $master multi
            $master select 0
            $master set key db0
            $master hmset myhash f1 v1 f2 v2 f3 v3
            $master select 1
            $master set key db1
            $master rpush mylist 1 2 3 4 5
            $master exec

            wait_for_ofs_sync $master $slave

            # wait_keyspace_cold $slave #TODO uncomment when list ready
            $slave select 0
            wait_key_cold $slave key
            wait_key_cold $slave myhash
            r select 1
            wait_key_cold r key

            $slave multi
            $slave select 1
            $slave lindex mylist 3
            $slave hmget myhash f1 f2
            $slave select 0
            $slave get key
            $slave hmget myhash f1 f2
            $slave select 1
            $slave get key
            assert_equal [$slave exec] {OK 4 {{} {}} OK db0 {v1 v2} OK db1}
        }

        test {multi-db gtid exec} {
            $master multi
            $master gtid A:11 1 set key6 db1
            $master gtid A:12 0 set key6 db0 
            $master select 0
            $master hmset myhash6 f1 db0 f2 db0
            $master gtid A:13 1 rpush mylist6 db1-1 db1-2 db1-3 db1-4
            $master gtid A:14 0 rpush mylist6 db0-1 db0-2 db0-3 db0-4
            $master select 1
            $master hmset myhash6 f1 db1 f2 db1
            $master gtid A:10 1 exec

            wait_for_ofs_sync $master $slave
            # wait_keyspace_cold $slave #TODO uncomment when list ready
            $slave select 0
            wait_key_cold $slave key6
            wait_key_cold $slave myhash6
            r select 1
            wait_key_cold r key6
            wait_key_cold r myhash6

            $slave select 0
            assert_equal [$slave get key6] db0
            assert_equal [$slave lindex mylist6 2] db0-3
            assert_equal [$slave hmget myhash6 f1 f2] {db0 db0}

            $slave select 1
            assert_equal [$slave get key6] db1
            assert_equal [$slave lindex mylist6 2] db1-3
            assert_equal [$slave hmget myhash6 f1 f2] {db1 db1}
        }
    }
}

start_server {overrides {gtid-enabled "yes"} tags {"swap" "select"}} {
    start_server {overrides {gtid-enabled "yes"}} {
        set master [srv -1 client]
        set master_host [srv -1 host]
        set master_port [srv -1 port]
        set slave [srv 0 client]

        test {multi-db repl by rdb} {
            for {set db 0} {$db < 5} {incr db} {
                $master select $db
                for {set count 0} {$count < 100} {incr count} {
                    $master set kv-$count db-$db 
                }

                for {set count 0} {$count < 100} {incr count} {
                    $master hmset hash-$count f1 db-$db f2 db-$db f3 db-$db f4 db-$db f5 db-$db f6 db-$db f7 db-$db f8 db-$db
                }
            }

            after 1000

            $slave slaveof $master_host $master_port
            wait_for_sync $slave

            for {set db 0} {$db < 5} {incr db} {
                $slave select $db
                for {set count 0} {$count < 100} {incr count} {
                    assert_equal [$slave get kv-$count] db-$db
                }

                for {set count 0} {$count < 100} {incr count} {
                    assert_equal [$slave hmget hash-$count f1 f8] [list db-$db db-$db]
                }
            }
        }

        test {multi-db multi/exec} {
            $master select 0
            # $master select 1 TODO uncomment 

            $master multi
            $master select 0
            $master set key db0
            $master hmset myhash f1 v1 f2 v2 f3 v3
            $master select 1
            $master set key db1
            $master rpush mylist 1 2 3 4 5
            $master exec

            wait_for_ofs_sync $master $slave

            wait_keyspace_cold $slave

            $slave multi
            $slave select 1
            $slave lindex mylist 3
            $slave hmget myhash f1 f2
            $slave select 0
            $slave get key
            $slave hmget myhash f1 f2
            $slave select 1
            $slave get key
            assert_equal [$slave exec] {OK 4 {{} {}} OK db0 {v1 v2} OK db1}
        }
    }
}

start_server {overrides {save ""} tags {"swap" "select"}} {
    start_server {} {

        set master [srv -1 client]
        set master_host [srv -1 host]
        set master_port [srv -1 port]
        set slave [srv 0 client]

        test {multi-db chaos load} {
            $slave slaveof $master_host $master_port
            wait_for_sync $slave

            set dbnum 3
            for {set db 0} {$db < $dbnum} {incr db} {
                set load_handle($db) [start_bg_complex_data $master_host $master_port $db 100000000]
            }

            after 10000

            for {set db 0} {$db < $dbnum} {incr db} {
                stop_bg_complex_data $load_handle($db)
            }

            wait_load_handlers_disconnected

            wait_for_ofs_sync $master $slave

            for {set db 0} {$db < $dbnum} {incr db} {
                $master select $db
                $slave select $db
                wait_for_condition 500 10 {
                    [$master dbsize] eq [$slave dbsize]
                } else {
                    fail "db$db dbsize not match"
                }
            }
        }
    }
}
