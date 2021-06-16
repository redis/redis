proc client_idle_sec {name} {
    set clients [split [r client list] "\r\n"]
    set c [lsearch -inline $clients *name=$name*]
    assert {[regexp {idle=([0-9]+)} $c - idle]}
    return $idle
}

# Calculate query buffer memory of slave
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
    # The test will run at least 2s to check if client query
    # buffer will be resized when client idle 2s.
    test "query buffer resized correctly" {
        # Memory will increase by more than 32k due to client query buffer.
        set rd [redis_deferring_client]
        $rd client setname test_client
        set orig_test_client_qbuf [client_query_buffer test_client]
        assert {$orig_test_client_qbuf >= 16384 && $orig_test_client_qbuf < 32768}

        # Check that the initial query buffer is not resized if it is idle for more than 2s
        wait_for_condition 1000 10 {
            [client_idle_sec test_client] > 3 && [client_query_buffer test_client] == $orig_test_client_qbuf
        } else {
            fail "query buffer was resized"
        }

        # Fill query buffer to more than 32k
        $rd set bigstring v ;# create bigstring in advance to avoid adding extra memory
        $rd set bigstring [string repeat A 32768] nx

        # Wait for query buffer to be resized to 0.
        wait_for_condition 1000 10 {
            [client_query_buffer test_client] == 0
        } else {
            fail "querybuf expected to be resized"
        }
    }
}
