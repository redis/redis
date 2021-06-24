proc client_field {name f} {
    set clients [split [r client list] "\r\n"]
    set c [lsearch -inline $clients *name=$name*]
    if {![regexp $f=(\[a-zA-Z0-9-\]+) $c - res]} {
        throw no-client "no client named $name found"
    }
    return $res
}

start_server {} {
    set maxmemory_clients 3000000
    r config set maxmemory-clients $maxmemory_clients
    
    test "client evicted due to query buf" {
        r flushdb
        set rr [redis_client]
        # Attempt a large multi-bulk command under eviction limit
        $rr mset k v k2 [string repeat v 1000000]
        assert_equal [$rr get k] v
        # Attempt another command, now causing client eviction
        catch { $rr mset k v k2 [string repeat v $maxmemory_clients] } e
        assert_match {*connection reset by peer*} $e
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
                assert_match {no client named test_client found} $e
                break
            }
        }
    }
}

proc mb {v} {
    return [expr $v * 1024 * 1024]
}

start_server {} {
    set server_pid [s process_id]
    set maxmemory_clients [mb 10]
    set obuf_limit [mb 3]
    r config set maxmemory-clients $maxmemory_clients
    r config set client-output-buffer-limit "normal $obuf_limit 0 0"

    test "avoid client eviction when client is freed by output buffer limit" {
        r flushdb
        set obuf_size [expr {$obuf_limit + [mb 1]}]
        r setrange k $obuf_size v
        set rr1 [redis_client]
        $rr1 client setname "qbuf-client"
        set rr2 [redis_deferring_client]
        $rr2 client setname "obuf-client1"
        assert_match [$rr2 read] OK
        set rr3 [redis_deferring_client]
        $rr3 client setname "obuf-client2"
        assert_match [$rr3 read] OK

        # Occupy client's query buff with less than output buffer limit left to exceed maxmemory-clients
        set qbsize [expr {$maxmemory_clients - $obuf_size}]
        $rr1 write [join [list "*1\r\n\$$qbsize\r\n" [string repeat v $qbsize]] ""]
        $rr1 flush
        # Wait for qbuff to be as expected
        wait_for_condition 200 10 {
            [client_field qbuf-client qbuf] == $qbsize
        } else {
            fail "Failed to fill qbuf for test"
        }
        
        # Make the other two obuf-clients pass obuf limit and also pass maxmemory-clients
        # We use two obuf-clients to make sure that even if client eviction is attempted
        # btween two command processing (with no sleep) we don't perform any client eviction
        # because the obuf limit is enforced with precendence.
        exec kill -SIGSTOP $server_pid
        $rr2 get k
        $rr2 flush
        $rr3 get k
        $rr3 flush
        exec kill -SIGCONT $server_pid
        
        # Validate obuf-clients were disconnected (because of obuf limit)
        catch {client_field obuf-client1 name} e
        assert_match $e {no client named obuf-client1 found}
        catch {client_field obuf-client2 name} e
        assert_match $e {no client named obuf-client2 found}
        
        # Validate qbuf-client is still connected and wasn't evicted
        assert_match [client_field qbuf-client name] {qbuf-client}
    }
}
