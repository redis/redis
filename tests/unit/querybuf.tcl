start_server {tags {"querybuf"}} {
    test "query buffer will never be resized when less than 64k" {
        set orig_used [s used_memory]

        # Memory will increase by more than 32k due to query buffer of client.
        set rd [redis_deferring_client]
        assert_morethan [expr [s used_memory] - $orig_used] 32768

        if {$::accurate} {
            after 2000 ;# Check if client query buffer will be resized when client idle 2s
        } else {
            after 120 ;# serverCron only call clientsCronResizeQueryBuffer once in 100ms
        }

        # Check query buffer of client has not been resized.
        assert_morethan [expr [s used_memory] - $orig_used] 32768
    }

    test "query buffer will be resized when more than 64k" {
        # The test will run at least 2s to wait for client query
        # buffer to be resized after idle 2s.
        if {$::accurate} {
            set rd [redis_deferring_client]
            set orig_used [s used_memory]

            # Fill client query buffer to more than 64k without adding extra memory
            $rd set bigstring v
            $rd set bigstring [string repeat A 65535] nx

            # Wait for client query buffer to be resized.
            # Memory will be reduced 32k with glibc and 40k with jemalloc.
            wait_for_condition 500 10 {
                [expr $orig_used - [s used_memory]] > 30000
            } else {
                fail "querybuf expected to be resized"
            }
        }
    }
}
