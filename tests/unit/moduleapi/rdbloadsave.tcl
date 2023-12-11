set testmodule [file normalize tests/modules/rdbloadsave.so]

start_server {tags {"modules"}} {
    r module load $testmodule

    test "Module rdbloadsave sanity" {
        r test.sanity

        # Try to load non-existing file
        assert_error {*No such file or directory*} {r test.rdbload sanity.rdb}

        r set x 1
        assert_equal OK [r test.rdbsave sanity.rdb]

        r flushdb
        assert_equal OK [r test.rdbload sanity.rdb]
        assert_equal 1 [r get x]
    }

    test "Module rdbloadsave test with pipelining" {
        r config set save ""
        r config set loading-process-events-interval-bytes 1024
        r config set key-load-delay 50
        r flushdb

        populate 3000 a 1024
        r set x 111
        assert_equal [r dbsize] 3001

        assert_equal OK [r test.rdbsave blabla.rdb]
        r flushdb
        assert_equal [r dbsize] 0

        # Send commands with pipeline. First command will call RM_RdbLoad() in
        # the command callback. While loading RDB, Redis can go to networking to
        # reply -LOADING. By sending commands in pipeline, we verify it doesn't
        # cause a problem.
        # e.g. Redis won't try to process next message of the current client
        # while it is in the command callback for that client   .
        set rd1 [redis_deferring_client]
        $rd1 test.rdbload blabla.rdb

        wait_for_condition 50 100 {
            [s loading] eq 1
        } else {
            fail "Redis did not start loading or loaded RDB too fast"
        }

        $rd1 get x
        $rd1 dbsize

        assert_equal OK [$rd1 read]
        assert_equal 111 [$rd1 read]
        assert_equal 3001 [$rd1 read]
        r flushdb
        r config set key-load-delay 0
    }

    test "Module rdbloadsave with aof" {
        r config set save ""

        # Enable the AOF
        r config set appendonly yes
        r config set auto-aof-rewrite-percentage 0 ; # Disable auto-rewrite.
        waitForBgrewriteaof r

        r set k v1
        assert_equal OK [r test.rdbsave aoftest.rdb]

        r set k v2
        r config set rdb-key-save-delay 10000000
        r bgrewriteaof

        # RM_RdbLoad() should kill aof fork
        assert_equal OK [r test.rdbload aoftest.rdb]

        wait_for_condition 50 100 {
            [string match {*Killing*AOF*child*} [exec tail -20 < [srv 0 stdout]]]
        } else {
            fail "Can't find 'Killing AOF child' in recent log lines"
        }

        # Verify the value in the loaded rdb
        assert_equal v1 [r get k]

        r flushdb
        r config set rdb-key-save-delay 0
        r config set appendonly no
    }

    test "Module rdbloadsave with bgsave" {
        r flushdb
        r config set save ""

        r set k v1
        assert_equal OK [r test.rdbsave bgsave.rdb]

        r set k v2
        r config set rdb-key-save-delay 500000
        r bgsave

        # RM_RdbLoad() should kill RDB fork
        assert_equal OK [r test.rdbload bgsave.rdb]

        wait_for_condition 10 1000 {
            [string match {*Background*saving*terminated*} [exec tail -20 < [srv 0 stdout]]]
        } else {
            fail "Can't find 'Background saving terminated' in recent log lines"
        }

        assert_equal v1 [r get k]
        r flushall
        waitForBgsave r
        r config set rdb-key-save-delay 0
    }

    test "Module rdbloadsave calls rdbsave in a module fork" {
        r flushdb
        r config set save ""
        r config set rdb-key-save-delay 500000

        r set k v1

        # Module will call RM_Fork() before calling RM_RdbSave()
        assert_equal OK [r test.rdbsave_fork rdbfork.rdb]
        assert_equal [s module_fork_in_progress] 1

        wait_for_condition 10 1000 {
            [status r module_fork_in_progress] == "0"
        } else {
            fail "Module fork didn't finish"
        }

        r set k v2
        assert_equal OK [r test.rdbload rdbfork.rdb]
        assert_equal v1 [r get k]

        r config set rdb-key-save-delay 0
    }

    test "Unload the module - rdbloadsave" {
        assert_equal {OK} [r module unload rdbloadsave]
    }

    tags {repl} {
        test {Module rdbloadsave on master and replica} {
            start_server [list overrides [list loadmodule "$testmodule"]] {
                set replica [srv 0 client]
                set replica_host [srv 0 host]
                set replica_port [srv 0 port]
                start_server [list overrides [list loadmodule "$testmodule"]] {
                    set master [srv 0 client]
                    set master_host [srv 0 host]
                    set master_port [srv 0 port]

                    $master set x 10000

                    # Start the replication process...
                    $replica replicaof $master_host $master_port

                    wait_for_condition 100 100 {
                        [status $master sync_full] == 1
                    } else {
                        fail "Master <-> Replica didn't start the full sync"
                    }

                    # RM_RdbSave() is allowed on replicas
                    assert_equal OK [$replica test.rdbsave rep.rdb]

                    # RM_RdbLoad() is not allowed on replicas
                    assert_error {*supported*} {$replica test.rdbload rep.rdb}

                    assert_equal OK [$master test.rdbsave master.rdb]
                    $master set x 20000

                    wait_for_condition 100 100 {
                        [$replica get x] == 20000
                    } else {
                        fail "Replica didn't get the update"
                    }

                    # Loading RDB on master will drop replicas
                    assert_equal OK [$master test.rdbload master.rdb]

                    wait_for_condition 100 100 {
                        [status $master sync_full] == 2
                    } else {
                        fail "Master <-> Replica didn't start the full sync"
                    }

                    wait_for_condition 100 100 {
                        [$replica get x] == 10000
                    } else {
                        fail "Replica didn't get the update"
                    }
                }
            }
        }
    }
}
