start_server {tags {"querybuf"}} {
    test "querybuf will never be resized when querybuf size less than 64k" {
        set orig_used [s used_memory]

        # memory will increase by more than 32k due to querybuf of new client.
        set rd [redis_deferring_client]
        assert_morethan [expr [s used_memory] - $orig_used] 32768

        after 1000

        # check querybuf of new client has not been resized.
        assert_morethan [expr [s used_memory] - $orig_used] 32768
    }

    test "querybuf will be resized when querybuf size more than 64k" {
        set rd1 [redis_deferring_client]
        $rd1 set bigstring [string repeat A 65535] ;# create a 64k bigstring in advance
        $rd1 close

        set rd2 [redis_deferring_client]
        set orig_used [s used_memory]
        $rd2 set bigstring [string repeat A 65535]
        wait_for_condition 500 10 {
            # memory will be reduced 32k with glibc and 40k with jemalloc
            [expr $orig_used - [s used_memory]] > 30000
        } else {
            fail "querybuf expected to be resized"
        }
    }
}
