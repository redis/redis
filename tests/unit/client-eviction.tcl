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

start_server {} {
    set maxmemory_clients 20000000
    set obuf_limit 6000000
    r config set maxmemory-clients $maxmemory_clients
    r config set client-output-buffer-limit "normal $obuf_limit 0 0"

    test "avoid client eviction when client is freed by output buffer limit" {
        r flushdb
        r setrange k 200000 v
        # Occupy client's query buff with half of maxmemory_clients
        set rr1 [redis_client]
        $rr1 client setname "qbuf-client"
        set rr2 [redis_deferring_client]
        set qbsize [expr {$maxmemory_clients - 5000000 }]
        $rr1 write [join [list "*1\r\n\$$qbsize\r\n" [string repeat v $qbsize]] ""]
        #$rr1 write [string repeat v [expr {100000 }]]
        $rr1 flush
        # Wait for qbuff to be as expected
        wait_for_condition 200 10 {
            [client_field qbuf-client qbuf] == $qbsize
        } else {
            fail "Failed to fill qbuf for test"
        }
        
        # Now we know that ~15mb is being used by rr1, fill rr2's obuf until obuf limit, hopefully we won't trigger rr1 to disconnect
        while true {
            if { [catch {
                $rr2 get k
                $rr2 flush
               } e]} {
                assert_match {I/O error reading reply} $e
                break
            }
        }
        
        # Validate rr1 is still connected
        assert_match [client_field qbuf-client name] {qbuf-client}
    }
}
