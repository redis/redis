proc client_idle_sec {name} {
    set clients [split [r client list] "\r\n"]
    set c [lsearch -inline $clients *name=$name*]
    assert {[regexp {idle=([0-9]+)} $c - idle]}
    return $idle
}

# Calculate query buffer memory of client
proc client_query_buffer {name} {
    set clients [split [r client list] "\r\n"]
    set c [lsearch -inline $clients *name=$name*]
    if {[string length $c] > 0} {
        assert {[regexp {qbuf=([0-9]+)} $c - qbuf]}
        assert {[regexp {qbuf-free=([0-9]+)} $c - qbuf_free]}
        return [expr $qbuf + $qbuf_free]
    }
    return 0
}

start_server {tags {"querybuf slow"}} {
    # increase the execution frequency of clientsCron
    r config set hz 100

    # The test will run at least 2s to check if client query
    # buffer will be resized when client idle 2s.
    test "query buffer resized correctly" {
        set rd [redis_client]
        $rd client setname test_client
        set orig_test_client_qbuf [client_query_buffer test_client]
        # Make sure query buff has less than the peak resize threshold (PROTO_RESIZE_THRESHOLD) 32k
        # but at least the basic IO reading buffer size (PROTO_IOBUF_LEN) 16k
        assert {$orig_test_client_qbuf >= 16384 && $orig_test_client_qbuf < 32768}

        # Check that the initial query buffer is resized after 2 sec
        wait_for_condition 1000 10 {
            [client_idle_sec test_client] >= 3 && [client_query_buffer test_client] == 0
        } else {
            fail "query buffer was not resized"
        }
        $rd close
    }

    test "query buffer resized correctly when not idle" {
        # Pause cron to prevent premature shrinking (timing issue).
        r debug pause-cron 1

        # Memory will increase by more than 32k due to client query buffer.
        set rd [redis_client]
        $rd client setname test_client

        # Create a large query buffer (more than PROTO_RESIZE_THRESHOLD - 32k)
        $rd set x [string repeat A 400000]

        # Make sure query buff is larger than the peak resize threshold (PROTO_RESIZE_THRESHOLD) 32k
        set orig_test_client_qbuf [client_query_buffer test_client]
        assert {$orig_test_client_qbuf > 32768}

        r debug pause-cron 0

        # Wait for qbuf to shrink due to lower peak
        set t [clock milliseconds]
        while true {
            # Write something smaller, so query buf peak can shrink
            $rd set x [string repeat A 100]
            set new_test_client_qbuf [client_query_buffer test_client]
            if {$new_test_client_qbuf < $orig_test_client_qbuf} { break } 
            if {[expr [clock milliseconds] - $t] > 1000} { break }
            after 10
        }
        # Validate qbuf shrunk but isn't 0 since we maintain room based on latest peak
        assert {[client_query_buffer test_client] > 0 && [client_query_buffer test_client] < $orig_test_client_qbuf}
        $rd close
    } {0} {needs:debug}

    test "query buffer resized correctly with fat argv" {
        set rd [redis_client]
        $rd client setname test_client
        $rd write "*3\r\n\$3\r\nset\r\n\$1\r\na\r\n\$1000000\r\n"
        $rd flush
        
        after 20
        if {[client_query_buffer test_client] < 1000000} {
            fail "query buffer should not be resized when client idle time smaller than 2s"
        }
     
        # Check that the query buffer is resized after 2 sec
        wait_for_condition 1000 10 {
            [client_idle_sec test_client] >= 3 && [client_query_buffer test_client] < 1000000
        } else {
            fail "query buffer should be resized when client idle time bigger than 2s"
        }
     
        $rd close
    }

}
