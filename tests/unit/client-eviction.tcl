tags {"external:skip logreqres:skip"} {

# Get info about a redis client connection:
# name - name of client we want to query
# f - field name from "CLIENT LIST" we want to get
proc client_field {name f} {
    set clients [split [string trim [r client list]] "\r\n"]
    set c [lsearch -inline $clients *name=$name*]
    if {![regexp $f=(\[a-zA-Z0-9-\]+) $c - res]} {
        error "no client named $name found with field $f"
    }
    return $res
}

proc client_exists {name} {
    if {[catch { client_field $name tot-mem } e]} {
        return false
    }
    return true
}

proc gen_client {} {
    set rr [redis_client]
    set name "tst_[randstring 4 4 simplealpha]"
    $rr client setname $name
    assert {[client_exists $name]}
    return [list $rr $name]
}

# Sum a value across all redis client connections:
# f - the field name from "CLIENT LIST" we want to sum
proc clients_sum {f} {
    set sum 0
    set clients [split [string trim [r client list]] "\r\n"]
    foreach c $clients {
        if {![regexp $f=(\[a-zA-Z0-9-\]+) $c - res]} {
            error "field $f not found in $c"
        }
        incr sum $res
    }
    return $sum
}

proc mb {v} {
    return [expr $v * 1024 * 1024]
}

proc kb {v} {
    return [expr $v * 1024]
}

start_server {} {
    set maxmemory_clients 3000000
    r config set maxmemory-clients $maxmemory_clients

    test "client evicted due to large argv" {
        r flushdb
        lassign [gen_client] rr cname
        # Attempt a large multi-bulk command under eviction limit
        $rr mset k v k2 [string repeat v 1000000]
        assert_equal [$rr get k] v
        # Attempt another command, now causing client eviction
        catch { $rr mset k v k2 [string repeat v $maxmemory_clients] } e
        assert {![client_exists $cname]}
        $rr close
    }

    test "client evicted due to large query buf" {
        r flushdb
        lassign [gen_client] rr cname
        # Attempt to fill the query buff without completing the argument above the limit, causing client eviction
        catch {
            $rr write [join [list "*1\r\n\$$maxmemory_clients\r\n" [string repeat v $maxmemory_clients]] ""]
            $rr flush
            $rr read
        } e
        assert {![client_exists $cname]}
        $rr close
    }

    test "client evicted due to percentage of maxmemory" {
        set maxmemory [mb 6]
        r config set maxmemory $maxmemory
        # Set client eviction threshold to 7% of maxmemory
        set maxmemory_clients_p 7
        r config set maxmemory-clients $maxmemory_clients_p%
        r flushdb

        set maxmemory_clients_actual [expr $maxmemory * $maxmemory_clients_p / 100]

        lassign [gen_client] rr cname
        # Attempt to fill the query buff with only half the percentage threshold verify we're not disconnected
        set n [expr $maxmemory_clients_actual / 2]
        $rr write [join [list "*1\r\n\$$n\r\n" [string repeat v $n]] ""]
        $rr flush
        wait_for_condition 100 10 {
            [client_field $cname tot-mem] >= $n
        } else {
            fail "Failed to fill qbuf for test"
        }
        set tot_mem [client_field $cname tot-mem]
        assert {$tot_mem >= $n && $tot_mem < $maxmemory_clients_actual}

        # Attempt to fill the query buff with the percentage threshold of maxmemory and verify we're evicted
        $rr close
        lassign [gen_client] rr cname
        catch {
            $rr write [join [list "*1\r\n\$$maxmemory_clients_actual\r\n" [string repeat v $maxmemory_clients_actual]] ""]
            $rr flush
        } e
        wait_for_condition 100 10 {
            ![client_exists $cname]
        } else {
            fail "Failed to evict client"
        }
        $rr close

        # Restore settings
        r config set maxmemory 0
        r config set maxmemory-clients $maxmemory_clients
    }

    test "client evicted due to large multi buf" {
        r flushdb
        lassign [gen_client] rr cname

        # Attempt a multi-exec where sum of commands is less than maxmemory_clients
        $rr multi
        $rr set k [string repeat v [expr $maxmemory_clients / 4]]
        $rr set k [string repeat v [expr $maxmemory_clients / 4]]
        assert_equal [$rr exec] {OK OK}

        # Attempt a multi-exec where sum of commands is more than maxmemory_clients, causing client eviction
        $rr multi
        catch {
            for {set j 0} {$j < 5} {incr j} {
                $rr set k [string repeat v [expr $maxmemory_clients / 4]]
            }
        } e
        assert {![client_exists $cname]}
        $rr close
    }

    test "client evicted due to watched key list" {
        r flushdb
        set rr [redis_client]

        # Since watched key list is a small overhead this test uses a minimal maxmemory-clients config
        set temp_maxmemory_clients 200000
        r config set maxmemory-clients $temp_maxmemory_clients

        # Append watched keys until list maxes out maxmemory clients and causes client eviction
        catch {
            for {set j 0} {$j < $temp_maxmemory_clients} {incr j} {
                $rr watch $j
            }
        } e
        assert_match {I/O error reading reply} $e
        $rr close

        # Restore config for next tests
        r config set maxmemory-clients $maxmemory_clients
    }

    test "client evicted due to pubsub subscriptions" {
        r flushdb

        # Since pubsub subscriptions cause a small overhead this test uses a minimal maxmemory-clients config
        set temp_maxmemory_clients 200000
        r config set maxmemory-clients $temp_maxmemory_clients

        # Test eviction due to pubsub patterns
        set rr [redis_client]
        # Add patterns until list maxes out maxmemory clients and causes client eviction
        catch {
            for {set j 0} {$j < $temp_maxmemory_clients} {incr j} {
                $rr psubscribe $j
            }
        } e
        assert_match {I/O error reading reply} $e
        $rr close

        # Test eviction due to pubsub channels
        set rr [redis_client]
        # Subscribe to global channels until list maxes out maxmemory clients and causes client eviction
        catch {
            for {set j 0} {$j < $temp_maxmemory_clients} {incr j} {
                $rr subscribe $j
            }
        } e
        assert_match {I/O error reading reply} $e
        $rr close

        # Test eviction due to sharded pubsub channels
        set rr [redis_client]
        # Subscribe to sharded pubsub channels until list maxes out maxmemory clients and causes client eviction
        catch {
            for {set j 0} {$j < $temp_maxmemory_clients} {incr j} {
                $rr ssubscribe $j
            }
        } e
        assert_match {I/O error reading reply} $e
        $rr close

        # Restore config for next tests
        r config set maxmemory-clients $maxmemory_clients
    }

    test "client evicted due to tracking redirection" {
        r flushdb
        set rr [redis_client]
        set redirected_c [redis_client]
        $redirected_c client setname redirected_client
        set redir_id [$redirected_c client id]
        $redirected_c SUBSCRIBE __redis__:invalidate
        $rr client tracking on redirect $redir_id bcast
        # Use a big key name to fill the redirected tracking client's buffer quickly
        set key_length [expr 1024*200]
        set long_key [string repeat k $key_length]
        # Use a script so we won't need to pass the long key name when dirtying it in the loop
        set script_sha [$rr script load "redis.call('incr', '$long_key')"]

        # Pause serverCron so it won't update memory usage since we're testing the update logic when
        # writing tracking redirection output
        r debug pause-cron 1

        # Read and write to same (long) key until redirected_client's buffers cause it to be evicted
        catch {
            while true {
                set mem [client_field redirected_client tot-mem]
                assert {$mem < $maxmemory_clients}
                $rr evalsha $script_sha 0
            }
        } e
        assert_match {no client named redirected_client found*} $e

        r debug pause-cron 0
        $rr close
        $redirected_c close
    } {0} {needs:debug}

    test "client evicted due to client tracking prefixes" {
        r flushdb
        set rr [redis_client]

        # Since tracking prefixes list is a small overhead this test uses a minimal maxmemory-clients config
        set temp_maxmemory_clients 200000
        r config set maxmemory-clients $temp_maxmemory_clients

        # Append tracking prefixes until list maxes out maxmemory clients and causes client eviction
        # Combine more prefixes in each command to speed up the test. Because we did not actually count
        # the memory usage of all prefixes, see getClientMemoryUsage, so we can not use larger prefixes
        # to speed up the test here.
        catch {
            for {set j 0} {$j < $temp_maxmemory_clients} {incr j} {
                $rr client tracking on prefix [format a%09s $j] prefix [format b%09s $j] prefix [format c%09s $j] bcast
            }
        } e
        assert_match {I/O error reading reply} $e
        $rr close

        # Restore config for next tests
        r config set maxmemory-clients $maxmemory_clients
    }

    test "client evicted due to output buf" {
        r flushdb
        r setrange k 200000 v
        set rr [redis_deferring_client]
        $rr client setname test_client
        $rr flush
        assert {[$rr read] == "OK"}
        # Attempt a large response under eviction limit
        $rr get k
        $rr flush
        assert {[string length [$rr read]] == 200001}
        set mem [client_field test_client tot-mem]
        assert {$mem < $maxmemory_clients}

        # Fill output buff in loop without reading it and make sure
        # we're eventually disconnected, but before reaching maxmemory_clients
        while true {
            if { [catch {
                set mem [client_field test_client tot-mem]
                assert {$mem < $maxmemory_clients}
                $rr get k
                $rr flush
               } e]} {
                assert {![client_exists test_client]}
                break
            }
        }
        $rr close
    }

    foreach {no_evict} {on off} {
        test "client no-evict $no_evict" {
            r flushdb
            r client setname control
            r client no-evict on ;# Avoid evicting the main connection
            lassign [gen_client] rr cname
            $rr client no-evict $no_evict

            # Overflow maxmemory-clients
            set qbsize [expr {$maxmemory_clients + 1}]
            if {[catch {
                $rr write [join [list "*1\r\n\$$qbsize\r\n" [string repeat v $qbsize]] ""]
                $rr flush
                wait_for_condition 200 10 {
                    [client_field $cname qbuf] == $qbsize
                } else {
                    fail "Failed to fill qbuf for test"
                }
            } e] && $no_evict == off} {
                assert {![client_exists $cname]}
            } elseif {$no_evict == on} {
                assert {[client_field $cname tot-mem] > $maxmemory_clients}
            }
            $rr close
        }
    }
}

} ;# tags

