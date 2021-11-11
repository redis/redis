start_server {tags {"repl external:skip"}} {
    set replica [srv 0 client]
    set replica_host [srv 0 host]
    set replica_port [srv 0 port]
    set replica_log [srv 0 stdout]
    start_server {} {
        set master [srv 0 client]
        set master_host [srv 0 host]
        set master_port [srv 0 port]

        # Set master and replica to use diskless replication on swapdb mode
        $master config set repl-diskless-sync yes
        $master config set repl-diskless-sync-delay 0
        $master config set save ""
        $master config set rdbcompression no
        $master debug populate 1000 master 100000
        $replica config set repl-diskless-load swapdb
        $replica config set save ""

        # Initial sync to have matching replids between master and replica
        $replica replicaof $master_host $master_port

        # Let replica finish initial sync with master
        wait_for_condition 100 100 {
            [s -1 master_link_status] eq "up"
        } else {
            fail "Master <-> Replica didn't finish sync"
        }

        # Set master with a slow rdb generation, so that we can easily intercept loading
        # 10ms per key, with 1000 keys is 10 seconds
        $master config set rdb-key-save-delay 10000

        # Force the replica to try another full sync (this time it will have matching master replid)
        $master multi
        $master client kill type replica
        # Fill replication backlog with new content
        $master config set repl-backlog-size 16384
        for {set keyid 0} {$keyid < 10} {incr keyid} {
            $master set "$keyid string_$keyid" [string repeat A 16384]
        }
        $master exec

        test {Diskless load swapdb (async_loading): replica enter async_loading} {
            wait_for_condition 100 100 {
                [s -1 async_loading] eq 1
            } else {
                fail "Replica didn't get into async_loading mode"
            }

            assert_equal [s -1 loading] 0
        }

        $replica config set repl-diskless-load disabled

        wait_for_condition 100 100 {
            [s -1 master_link_status] eq "up"
        } else {
            fail "Master <-> Replica didn't finish sync"
        }
 
        assert_equal [$master debug digest] [$replica debug digest]
    }
}


