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

start_server {tags {"querybuf"}} {
    # The test will run at least 2s to check if client query
    # buffer will be resized when client idle 2s.
    if {$::accurate} {
        test "query buffer will never be resized when less than 32k" {
            # Memory will increase by more than 32k due to query buffer of client.
            set rd [redis_deferring_client]
            $rd client setname test_client
            set orig_test_client_qbuf [client_query_buffer test_client]

            # Check if client query buffer will be resized when client idle more than 2s
            wait_for_condition 1000 10 {
                [client_idle_sec test_client] > 3 && [client_query_buffer test_client] == $orig_test_client_qbuf
            } else {
                fail "query buffer was resized"
            }
        }
    }
}

start_server {tags {"querybuf"}} {
    # The test will run at least 2s to wait for client query
    # buffer to be resized after idle 2s.
    if {$::accurate} {
        test "query buffer will be resized when more than 32k" {
            set rd [redis_deferring_client]
            $rd client setname test_client
            assert_morethan_equal [client_query_buffer test_client] 16384

            # Fill client query buffer to more than 32k without adding extra memory
            $rd set bigstring v
            $rd set bigstring [string repeat A 32768] nx

            # Wait for client query buffer to be resized to 0.
            wait_for_condition 1000 10 {
                [client_query_buffer test_client] == 0
            } else {
                fail "querybuf expected to be resized"
            }
        }
    }
}
