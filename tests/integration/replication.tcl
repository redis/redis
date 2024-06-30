
test {diskless loading short read} {
    start_server {tags {"repl"} overrides {save ""}} {
        set replica [srv 0 client]
        set replica_host [srv 0 host]
        set replica_port [srv 0 port]
        start_server {overrides {save ""}} {
            set master [srv 0 client]
            set master_host [srv 0 host]
            set master_port [srv 0 port]

            # Set master and replica to use diskless replication
            $master config set repl-diskless-sync yes
            $master config set rdbcompression no
            $replica config set repl-diskless-load swapdb
            $master config set hz 500
            $replica config set hz 500
            $master config set dynamic-hz no
            $replica config set dynamic-hz no
            # Try to fill the master with all types of data types / encodings
            set start [clock clicks -milliseconds]

            # Set a function value to check short read handling on functions
            r function load {#!lua name=test
                redis.register_function('test', function() return 'hello1' end)
            }

            for {set k 0} {$k < 3} {incr k} {
                for {set i 0} {$i < 10} {incr i} {
                    r set "$k int_$i" [expr {int(rand()*10000)}]
                    r expire "$k int_$i" [expr {int(rand()*10000)}]
                    r set "$k string_$i" [string repeat A [expr {int(rand()*1000000)}]]
                    r hset "$k hash_small" [string repeat A [expr {int(rand()*10)}]]  0[string repeat A [expr {int(rand()*10)}]]
                    r hset "$k hash_large" [string repeat A [expr {int(rand()*10000)}]] [string repeat A [expr {int(rand()*1000000)}]]
                    r sadd "$k set_small" [string repeat A [expr {int(rand()*10)}]]
                    r sadd "$k set_large" [string repeat A [expr {int(rand()*1000000)}]]
                    r zadd "$k zset_small" [expr {rand()}] [string repeat A [expr {int(rand()*10)}]]
                    r zadd "$k zset_large" [expr {rand()}] [string repeat A [expr {int(rand()*1000000)}]]
                    r lpush "$k list_small" [string repeat A [expr {int(rand()*10)}]]
                    r lpush "$k list_large" [string repeat A [expr {int(rand()*1000000)}]]
                    for {set j 0} {$j < 10} {incr j} {
                        r xadd "$k stream" * foo "asdf" bar "1234"
                    }
                    r xgroup create "$k stream" "mygroup_$i" 0
                    r xreadgroup GROUP "mygroup_$i" Alice COUNT 1 STREAMS "$k stream" >
                }
            }

            if {$::verbose} {
                set end [clock clicks -milliseconds]
                set duration [expr $end - $start]
                puts "filling took $duration ms (TODO: use pipeline)"
                set start [clock clicks -milliseconds]
            }

            # Start the replication process...
            set loglines [count_log_lines -1]
            $master config set repl-diskless-sync-delay 0
            $replica replicaof $master_host $master_port

            # kill the replication at various points
            set attempts 100
            if {$::accurate} { set attempts 2500 }
            for {set i 0} {$i < $attempts} {incr i} {
                # wait for the replica to start reading the rdb
                # using the log file since the replica only responds to INFO once in 2mb
                set res [wait_for_log_messages -1 {"*Loading DB in memory*"} $loglines 2000 1]
                set loglines [lindex $res 1]

                # add some additional random sleep so that we kill the master on a different place each time
                after [expr {int(rand()*50)}]

                # kill the replica connection on the master
                set killed [$master client kill type replica]

                set res [wait_for_log_messages -1 {"*Internal error in RDB*" "*Finished with success*" "*Successful partial resynchronization*"} $loglines 500 10]
                if {$::verbose} { puts $res }
                set log_text [lindex $res 0]
                set loglines [lindex $res 1]
                if {![string match "*Internal error in RDB*" $log_text]} {
                    # force the replica to try another full sync
                    $master multi
                    $master client kill type replica
                    $master set asdf asdf
                    # fill replication backlog with new content
                    $master config set repl-backlog-size 16384
                    for {set keyid 0} {$keyid < 10} {incr keyid} {
                        $master set "$keyid string_$keyid" [string repeat A 16384]
                    }
                    $master exec
                }

                # wait for loading to stop (fail)
                # After a loading successfully, next loop will enter `async_loading`
                wait_for_condition 1000 1 {
                    [s -1 async_loading] eq 0 &&
                    [s -1 loading] eq 0
                } else {
                    fail "Replica didn't disconnect"
                }
            }
            if {$::verbose} {
                set end [clock clicks -milliseconds]
                set duration [expr $end - $start]
                puts "test took $duration ms"
            }
            # enable fast shutdown
            $master config set rdb-key-save-delay 0
        }
    }
} {} {external:skip}

